import type { EngineEnvelope } from "./types";
import type {
  AxisMode,
  BackendPreference,
  CandidateGridSpec,
  KernelRef,
  MathMode,
  MetricSpec,
  SearchPreset,
} from "./protocol";
import { buildCandidateGrid, workEstimate } from "./candidateGrid";

export type HeightDraft = {
  subroute: "height" | "kernel";
  preset: SearchPreset;
  axisMode: AxisMode;
  start: string;
  stop: string;
  step: string;
  /** Used only by fractional_refine. */
  refineSelected: string;
  refineHalfSpan: string;
  kernelId: string;
  kernelParameters: Record<string, string | number | boolean>;
  compareCommonKernels: boolean;
  profileId: string;
  mathMode: MathMode;
  backendPreference: BackendPreference;
  metric: MetricSpec;
  /** Optional base height for fractional geometry. */
  baseHeight: string;
  baseWidth: string;
};

export function defaultHeightDraft(capabilities: EngineEnvelope | null): HeightDraft {
  const kernels = capabilities?.payload.kernels ?? [];
  const profiles = capabilities?.payload.profiles ?? [];
  const bicubic = kernels.find((kernel) => kernel.id === "bicubic");
  const defaultKernel = bicubic ?? kernels[0];
  const defaultProfile =
    profiles.find((profile) => profile.id.includes("muf"))?.id ??
    profiles[0]?.id ??
    "muf-d278cd3";

  return {
    subroute: "height",
    preset: "integer_coarse",
    axisMode: "h_only",
    start: "500",
    stop: "800",
    step: "1",
    refineSelected: "720",
    refineHalfSpan: "1.0",
    kernelId: defaultKernel?.id ?? "bicubic",
    kernelParameters: { ...(defaultKernel?.parameters ?? { b: 1 / 3, c: 1 / 3 }) },
    compareCommonKernels: false,
    profileId: defaultProfile,
    mathMode: "raw",
    backendPreference: "auto",
    metric: {
      cropLeft: 0,
      cropRight: 0,
      cropTop: 0,
      cropBottom: 0,
      pixelExclusionThreshold: 0,
      // getnative's default metric is the mean absolute retained error (p = 1),
      // which is also what the worker protocol v1 CPU backend supports.
      pNorm: 1,
    },
    baseHeight: "",
    baseWidth: "",
  };
}

export function applyPreset(draft: HeightDraft, preset: SearchPreset): HeightDraft {
  if (preset === "integer_coarse") {
    return {
      ...draft,
      preset,
      start: "500",
      stop: "800",
      step: "1",
    };
  }
  if (preset === "fractional_refine") {
    return {
      ...draft,
      preset,
      step: "0.1",
      refineHalfSpan: draft.refineHalfSpan || "1.0",
      refineSelected: draft.refineSelected || "720",
    };
  }
  return { ...draft, preset: "custom" };
}

export function resolveHeightGrid(
  draft: HeightDraft,
): { ok: true; grid: CandidateGridSpec } | { ok: false; reason: string } {
  if (draft.preset === "fractional_refine") {
    const selected = Number(draft.refineSelected);
    const half = Number(draft.refineHalfSpan);
    if (!Number.isFinite(selected) || !Number.isFinite(half)) {
      return { ok: false, reason: "refine_inputs_invalid" };
    }
    const start = (selected - half).toFixed(draft.step.includes(".") ? draft.step.split(".")[1].length : 1);
    const stop = (selected + half).toFixed(draft.step.includes(".") ? draft.step.split(".")[1].length : 1);
    return buildCandidateGrid({
      axis: "height",
      start,
      stop,
      step: draft.step,
      endpointRule: "inclusive",
      preset: "fractional_refine",
    });
  }
  return buildCandidateGrid({
    axis: "height",
    start: draft.start,
    stop: draft.stop,
    step: draft.step,
    endpointRule: "inclusive",
    preset: draft.preset,
  });
}

export function fixedKernelsForDraft(
  draft: HeightDraft,
  capabilities: EngineEnvelope | null,
): KernelRef[] {
  const primary: KernelRef = {
    id: draft.kernelId,
    parameters: { ...draft.kernelParameters },
  };
  if (!draft.compareCommonKernels) return [primary];
  const known = capabilities?.payload.kernels ?? [];
  const commonIds = ["bilinear", "bicubic", "lanczos", "spline36"];
  const fromCaps = commonIds
    .map((id) => known.find((kernel) => kernel.id === id))
    .filter((kernel): kernel is NonNullable<typeof kernel> => Boolean(kernel))
    .map((kernel) => ({ id: kernel.id, parameters: { ...kernel.parameters } }));
  if (fromCaps.length === 0) return [primary];
  // Ensure primary is first and unique by id.
  const rest = fromCaps.filter((kernel) => kernel.id !== primary.id);
  return [primary, ...rest];
}

export function estimateHeightWork(
  draft: HeightDraft,
  sampleCount: number,
  capabilities: EngineEnvelope | null,
): { ok: true; candidateCount: number; kernelCount: number; estimate: number } | { ok: false; reason: string } {
  const grid = resolveHeightGrid(draft);
  if (!grid.ok) return grid;
  const kernels = fixedKernelsForDraft(draft, capabilities);
  const estimate = workEstimate({
    sampleCount,
    fixedKernelCount: kernels.length,
    candidateCount: grid.grid.candidates.length,
  });
  return {
    ok: true,
    candidateCount: grid.grid.candidates.length,
    kernelCount: kernels.length,
    estimate,
  };
}

export function selectableBackends(capabilities: EngineEnvelope | null): BackendPreference[] {
  const backends = capabilities?.payload.backends ?? [];
  const options: BackendPreference[] = ["auto"];
  const cpu = backends.find((backend) => backend.id === "cpu");
  if (cpu?.compiled && cpu.device_available) options.push("cpu");
  const metal = backends.find((backend) => backend.id === "metal");
  if (metal?.compiled && metal.device_available) options.push("metal");
  // CUDA/Vulkan remain reserved capability slots; never offered as runnable preferences here.
  return options;
}
