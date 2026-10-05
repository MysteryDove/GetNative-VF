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
  TransferCurve,
} from "./protocol";
import { buildCandidateGrid, workEstimate } from "./candidateGrid";
import { MUF_PROFILE_ID, profileFor } from "./profiles";
import { hasFractionalPart, parseNumberInput } from "./numberInput";

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
  /** Linear-light hypothesis for the whole scan; `none` analyses as encoded. */
  transfer?: TransferCurve;
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
    transfer: "none",
  };
}

export function applyPreset(draft: HeightDraft, preset: SearchPreset): HeightDraft {
  if (preset === "integer_coarse") {
    // Broad scan keeps whatever range/step the user already entered; only an
    // empty field falls back to the profile-independent defaults.
    return {
      ...draft,
      preset,
      start: draft.start.trim() || "500",
      stop: draft.stop.trim() || "1000",
      endpointRule: "inclusive",
      step: draft.step.trim() || "1",
    };
  }
  if (preset === "fractional_refine") {
    return withFractionalBase({
      ...draft,
      preset,
      step: hasFractionalPart(draft.step) ? draft.step : "0.1",
      endpointRule: "inclusive",
      refineHalfSpan: draft.refineHalfSpan || "1.0",
      refineSelected: draft.refineSelected || "720",
    });
  }
  return { ...draft, preset: "custom" };
}

type FractionalBaseFields = Pick<
  HeightDraft,
  | "preset"
  | "axisMode"
  | "start"
  | "stop"
  | "step"
  | "refineSelected"
  | "refineHalfSpan"
  | "baseHeight"
  | "baseWidth"
  | "baseHeightMode"
  | "baseWidthMode"
>;

/** True when the scan produces (or is meant to produce) non-integer candidates. */
export function scansFractionalCandidates(
  draft: Pick<FractionalBaseFields, "preset" | "start" | "stop" | "step" | "refineSelected" | "refineHalfSpan">,
): boolean {
  if (draft.preset === "fractional_refine") return true;
  return [draft.start, draft.stop, draft.step].some(hasFractionalPart);
}

/**
 * Entering a decimal scan with an integer (null) base would collapse adjacent
 * candidates, so the scanned axis moves to an even base — the getnative
 * convention — unless the user already chose a parity or an explicit base.
 */
export function withFractionalBase<T extends FractionalBaseFields>(draft: T): T {
  const axis = missingFractionalBaseAxis(draft);
  if (axis === "height") return { ...draft, baseHeightMode: "even" };
  if (axis === "width") return { ...draft, baseWidthMode: "even" };
  return draft;
}

/** Which source dimension the scan range (start/stop/selected) is measured in. */
export type ScanAxisKind = "height" | "width";

export function scanAxisKind(axisMode: AxisMode): ScanAxisKind {
  return axisMode === "w_only" ? "width" : "height";
}

/** The axis-specific part of the draft: these numbers mean px of one axis. */
export type ScanRange = Pick<HeightDraft, "start" | "stop" | "refineSelected">;

/**
 * Translate a range to the other axis by the source aspect ratio
 * (500–1000 high on 16:9 → 889–1778 wide), so switching the scan axis lands
 * on the equivalent range instead of reusing height numbers as widths.
 * `ratio` is target/source size; unparsable fields pass through unchanged.
 */
export function convertScanRange(range: ScanRange, ratio: number): ScanRange {
  const convert = (text: string) => {
    const value = Number(text);
    if (!text.trim() || !Number.isFinite(value) || !(ratio > 0)) return text;
    return String(Math.round(value * ratio));
  };
  return {
    start: convert(range.start),
    stop: convert(range.stop),
    refineSelected: convert(range.refineSelected),
  };
}

/** The scanned axis must have an explicit base for decimal candidates to stay fractional. */
export function missingFractionalBaseAxis(
  draft: FractionalBaseFields,
): "height" | "width" | null {
  if (!scansFractionalCandidates(draft)) return null;
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

/** Numeric kernel parameters the user types as text (decimals or `p/q`). */
const NUMERIC_KERNEL_PARAMETERS = ["b", "c", "blur"] as const;

/** Names of typed kernel parameters that do not parse as numbers. */
export function invalidKernelParameterNames(
  parameters: Record<string, string | number | boolean> | undefined,
): string[] {
  if (!parameters) return [];
  return NUMERIC_KERNEL_PARAMETERS.filter(
    (name) => parameters[name] !== undefined && parseNumberInput(parameters[name]) === null,
  );
}

/** Resolve typed parameter text ("1/3", "0.5") into the numbers the engine takes. */
export function resolveKernelParameters(
  parameters: Record<string, string | number | boolean>,
): Record<string, string | number | boolean> {
  const resolved = { ...parameters };
  for (const name of NUMERIC_KERNEL_PARAMETERS) {
    if (resolved[name] === undefined) continue;
    const value = parseNumberInput(resolved[name]);
    if (value !== null) resolved[name] = value;
  }
  return resolved;
}

export function fixedKernelsForDraft(
  draft: HeightDraft,
  capabilities: EngineEnvelope | null,
): KernelRef[] {
  const primary: KernelRef = {
    id: draft.kernelId,
    parameters: resolveKernelParameters(draft.kernelParameters),
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
    if (parameters.blur === undefined && primary.parameters.blur !== undefined) {
      parameters.blur = primary.parameters.blur;
    }
    extras.push({
      id: kernel.id,
      parameters,
    });
  }
  return [primary, ...extras];
}

/** Mirrors the engine's `maximum_filter_blur`: plan size grows with blur. */
export const MAXIMUM_KERNEL_BLUR = 16;

export function invalidKernelBlur(
  parameters: Record<string, string | number | boolean> | undefined,
): boolean {
  if (parameters === undefined || parameters.blur === undefined) return false;
  const blur = parseNumberInput(parameters.blur);
  return blur === null || blur <= 0 || blur > MAXIMUM_KERNEL_BLUR;
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
