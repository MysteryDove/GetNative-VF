import type { EngineEnvelope } from "./types";
import type {
  AxisMode,
  BackendPreference,
  CandidateGridSpec,
  KernelRef,
  MathMode,
  MetricSpec,
  SearchPreset,
  EndpointRule,
  BaseMode,
} from "./protocol";
import { buildCandidateGrid, workEstimate } from "./candidateGrid";
import { MUF_PROFILE_ID, profileFor } from "./profiles";

export const CUDA_MAXIMUM_P_NORM = 4;

export type HeightDraft = {
  preset: SearchPreset;
  axisMode: AxisMode;
  start: string;
  stop: string;
  step: string;
  endpointRule: EndpointRule;
  /** Used only by fractional_refine. */
  refineSelected: string;
  refineHalfSpan: string;
  kernelId: string;
  kernelParameters: Record<string, string | number | boolean>;
  /** Additional kernels to compare against the fixed kernel (RunGroup members).
   *  Entries may carry parameter variants, e.g. lanczos with taps 2..6. */
  compareKernels: KernelRef[];
  profileId: string;
  mathMode: MathMode;
  backendPreference: BackendPreference;
  metric: MetricSpec;
  /** Optional base height for fractional geometry. */
  baseHeight: string;
  baseWidth: string;
  baseHeightMode: BaseMode;
  baseWidthMode: BaseMode;
};

export function defaultHeightDraft(capabilities: EngineEnvelope | null): HeightDraft {
  return heightDraftForProfile(capabilities);
}

function kernelParametersForProfile(
  profile: ReturnType<typeof profileFor>,
): Record<string, string | number | boolean> {
  if (profile.default_kernel.id === "bicubic") {
    return { b: profile.default_kernel.b, c: profile.default_kernel.c };
  }
  if (profile.default_kernel.id === "lanczos") {
    return { taps: profile.default_kernel.taps };
  }
  return {};
}

export function heightDraftForProfile(
  capabilities: EngineEnvelope | null,
): HeightDraft {
  const profile = profileFor(MUF_PROFILE_ID, capabilities);
  return {
    preset: "integer_coarse",
    axisMode: profile.default_axis_mode,
    start: profile.default_grid.start,
    stop: profile.default_grid.stop,
    step: profile.default_grid.step,
    endpointRule: "inclusive",
    refineSelected: "720",
    refineHalfSpan: "1.0",
    kernelId: profile.default_kernel.id,
    kernelParameters: kernelParametersForProfile(profile),
    compareKernels: [],
    profileId: profile.id,
    mathMode: "raw",
    backendPreference: "auto",
    metric: {
      cropLeft: profile.default_crop,
      cropRight: profile.default_crop,
      cropTop: profile.default_crop,
      cropBottom: profile.default_crop,
      pixelExclusionThreshold: profile.default_threshold,
      pNorm: 1,
    },
    baseHeight: "",
    baseWidth: "",
    baseHeightMode: "integer",
    baseWidthMode: "integer",
  };
}

export function applyPreset(draft: HeightDraft, preset: SearchPreset): HeightDraft {
  if (preset === "integer_coarse") {
    return {
      ...draft,
      preset,
      start: "500",
      stop: "1000",
      endpointRule: "inclusive",
      step: "1",
    };
  }
  if (preset === "fractional_refine") {
    return {
      ...draft,
      preset,
      step: "0.1",
      endpointRule: "inclusive",
      refineHalfSpan: draft.refineHalfSpan || "1.0",
      refineSelected: draft.refineSelected || "720",
    };
  }
  return { ...draft, preset: "custom" };
}

/** The scanned axis must have an explicit base for decimal candidates to stay fractional. */
export function missingFractionalBaseAxis(
  draft: Pick<HeightDraft, "preset" | "axisMode" | "baseHeight" | "baseWidth" | "baseHeightMode" | "baseWidthMode">,
): "height" | "width" | null {
  if (draft.preset !== "fractional_refine") return null;
  if (draft.axisMode === "w_only") {
    return draft.baseWidthMode !== "integer" || draft.baseWidth.trim() ? null : "width";
  }
  return draft.baseHeightMode !== "integer" || draft.baseHeight.trim() ? null : "height";
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
      axis: draft.axisMode === "w_only" ? "width" : "height",
      start,
      stop,
      step: draft.step,
      endpointRule: "inclusive",
      gridSemantics: profileFor(draft.profileId).grid_semantics,
      preset: "fractional_refine",
    });
  }
  return buildCandidateGrid({
    axis: draft.axisMode === "w_only" ? "width" : "height",
    start: draft.start,
    stop: draft.stop,
    step: draft.step,
    endpointRule: "inclusive",
    gridSemantics: profileFor(draft.profileId).grid_semantics,
    preset: draft.preset,
  });
}

/** Identity for dedup: same id AND same parameters collapse to one member. */
export function kernelSignature(kernel: KernelRef): string {
  return `${kernel.id}:${JSON.stringify(kernel.parameters)}`;
}

export function fixedKernelsForDraft(
  draft: HeightDraft,
  capabilities: EngineEnvelope | null,
): KernelRef[] {
  const primary: KernelRef = {
    id: draft.kernelId,
    parameters: { ...draft.kernelParameters },
  };
  const seen = new Set<string>([kernelSignature(primary)]);
  const extras: KernelRef[] = [];
  for (const kernel of draft.compareKernels) {
    const signature = kernelSignature(kernel);
    if (seen.has(signature)) continue;
    seen.add(signature);
    const known = capabilities?.payload.kernels.find(
      (candidate) => candidate.id === kernel.id,
    );
    const parameters = { ...(known?.parameters ?? {}), ...kernel.parameters };
    if (parameters.blur === undefined && draft.kernelParameters.blur !== undefined) {
      parameters.blur = draft.kernelParameters.blur;
    }
    extras.push({
      id: kernel.id,
      parameters,
    });
  }
  return [primary, ...extras];
}

export function invalidKernelBlur(
  parameters: Record<string, string | number | boolean> | undefined,
): boolean {
  if (parameters === undefined || parameters.blur === undefined) return false;
  const blur = Number(parameters.blur);
  return !Number.isFinite(blur) || blur <= 0;
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

export {
  resolveBackendPreference,
  selectableBackends,
  validateBackendPNorm,
} from "./backendSelection";
