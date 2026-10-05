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
import { decimalPlaces, hasFractionalPart, parseNumberInput } from "./numberInput";

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

/**
 * The scan mode is the preset: `integer_coarse` is a plain integer descale
 * (integer candidates, no base), `fractional_refine` a fractional one
 * (decimal candidates centred on an even/odd canvas).
 */
export function isIntegerScan(draft: Pick<HeightDraft, "preset">): boolean {
  return draft.preset !== "fractional_refine";
}

/** Round a typed number to an integer; unparsable text is left alone. */
export function roundIntegerText(text: string, minimum?: number): string {
  if (!text.trim()) return text;
  const value = Number(text);
  if (!Number.isFinite(value)) return text;
  const rounded = Math.round(value);
  return String(minimum == null ? rounded : Math.max(minimum, rounded));
}

/** Integer mode: every range field is an integer and no base is sent. */
function withIntegerScan<T extends HeightDraft>(draft: T): T {
  return {
    ...draft,
    start: roundIntegerText(draft.start),
    stop: roundIntegerText(draft.stop),
    step: roundIntegerText(draft.step, 1),
    baseHeight: "",
    baseWidth: "",
    baseHeightMode: "integer",
    baseWidthMode: "integer",
  };
}

export function applyPreset(draft: HeightDraft, preset: SearchPreset): HeightDraft {
  if (preset === "fractional_refine") {
    return withFractionalBase({
      ...draft,
      preset,
      start: draft.start.trim() || "500",
      stop: draft.stop.trim() || "1000",
      step: draft.step.trim() || "0.1",
      endpointRule: "inclusive",
    });
  }
  return withIntegerScan({
    ...draft,
    preset: "integer_coarse",
    start: draft.start.trim() || "500",
    stop: draft.stop.trim() || "1000",
    step: draft.step.trim() || "1",
    endpointRule: "inclusive",
  });
}

/**
 * Drafts saved before the integer / non-integer modes could hold decimals or
 * a parity base under the integer preset; those are non-integer scans.
 */
export function normalizeScanMode(draft: HeightDraft): HeightDraft {
  if (draft.preset === "fractional_refine") return withFractionalBase(draft);
  const fractional = [draft.start, draft.stop, draft.step].some(hasFractionalPart)
    || draft.baseHeightMode !== "integer" || draft.baseWidthMode !== "integer"
    || Boolean(draft.baseHeight.trim()) || Boolean(draft.baseWidth.trim());
  return fractional
    ? withFractionalBase({ ...draft, preset: "fractional_refine" })
    : { ...draft, preset: "integer_coarse" };
}

/**
 * Non-integer range centred on a picked value (the "refine around selection"
 * step): ±halfSpan at the current decimal step, or 0.1 coming from integers.
 */
export function refineAround(draft: HeightDraft, selected: string, halfSpan = 1): HeightDraft {
  const centre = Number(selected);
  if (!Number.isFinite(centre)) return draft;
  const step = hasFractionalPart(draft.step) ? draft.step : "0.1";
  const places = Math.max(decimalPlaces(step), decimalPlaces(selected));
  return applyPreset(
    {
      ...draft,
      start: (centre - halfSpan).toFixed(places),
      stop: (centre + halfSpan).toFixed(places),
      step,
    },
    "fractional_refine",
  );
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

/** True for a non-integer scan (decimal candidates on a parity canvas). */
export function scansFractionalCandidates(draft: Pick<HeightDraft, "preset">): boolean {
  return !isIntegerScan(draft);
}

/**
 * A non-integer scan with an integer (null) base would collapse adjacent
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
  // Integer mode never produces decimals, whatever was typed.
  const integer = isIntegerScan(draft);
  return buildCandidateGrid({
    axis: draft.axisMode === "w_only" ? "width" : "height",
    start: integer ? roundIntegerText(draft.start) : draft.start,
    stop: integer ? roundIntegerText(draft.stop) : draft.stop,
    step: integer ? roundIntegerText(draft.step, 1) : draft.step,
    endpointRule: "inclusive",
    gridSemantics: profileFor(draft.profileId).grid_semantics,
    preset: integer ? "integer_coarse" : "fractional_refine",
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
  _capabilities?: EngineEnvelope | null,
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
    // Capability `parameters` describe a kernel family (`kind`, limits), not
    // values to run with, so only the compare entry's own parameters count.
    const parameters = { ...kernel.parameters };
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
