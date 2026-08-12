import type { EngineEnvelope } from "./types";
import type {
  BackendPreference,
  GeometrySnapshot,
  KernelRef,
  MathMode,
  MetricSpec,
} from "./protocol";
import { buildCandidateGrid } from "./candidateGrid";
import { kernelSignature } from "./heightDraft";

/**
 * Kernel Analysis (算法测试) draft: one fixed geometry per Sample source shape,
 * plus a user-built scan list of kernel candidates. Geometry is never edited
 * here — it is resolved from the engine `geometry` command and displayed
 * read-only.
 */

/** Fixed deterministic family order; the add-form family picker is user-visible. */
export const KERNEL_FAMILY_ORDER = [
  "bilinear",
  "bicubic",
  "spline16",
  "spline36",
  "spline64",
  "lanczos",
] as const;

export type KernelFamilyId = (typeof KERNEL_FAMILY_ORDER)[number];

export type KernelDraft = {
  /** Ordered user-built candidate list; this IS the scan candidate set. */
  scanList: KernelRef[];
  /** Add-form state (not part of the candidate set). */
  addFamily: KernelFamilyId;
  /** (b, c) for a single Bicubic add. */
  bicubicB: string;
  bicubicC: string;
  /** Multi-selected taps for a Lanczos add. */
  lanczosTapsSelection: number[];
  /** Bicubic grid sweep ranges (decimal strings). */
  bStart: string;
  bStop: string;
  bStep: string;
  cStart: string;
  cStop: string;
  cStep: string;
  /** Target native height used to resolve the fixed geometry per source shape. */
  baseHeight: string;
  baseWidth: string;
  metric: MetricSpec;
  profileId: string;
  mathMode: MathMode;
  backendPreference: BackendPreference;
};

export function defaultKernelDraft(
  capabilities: EngineEnvelope | null,
  metric: MetricSpec,
  profileId: string,
  mathMode: MathMode,
  backendPreference: BackendPreference,
  base?: { baseHeight?: string; baseWidth?: string },
): KernelDraft {
  void capabilities;
  return {
    // Seed the six preset families so the panel opens runnable; every entry
    // is removable.
    scanList: [
      { id: "bilinear", parameters: {} },
      { id: "bicubic", parameters: { b: 0, c: 0.5 } },
      { id: "spline16", parameters: {} },
      { id: "spline36", parameters: {} },
      { id: "spline64", parameters: {} },
      { id: "lanczos", parameters: { taps: 3 } },
    ],
    addFamily: "bilinear",
    bicubicB: "0",
    bicubicC: "0.5",
    lanczosTapsSelection: [3],
    bStart: "0",
    bStop: "1",
    bStep: "0.2",
    cStart: "0",
    cStop: "1",
    cStep: "0.2",
    baseHeight: base?.baseHeight ?? "720",
    baseWidth: base?.baseWidth ?? "",
    metric: { ...metric },
    profileId,
    mathMode,
    backendPreference,
  };
}

function parseNonNegativeDecimal(value: string): number | null {
  const trimmed = value.trim();
  if (!/^\d+(\.\d+)?$/.test(trimmed)) return null;
  const n = Number(trimmed);
  return Number.isFinite(n) && n >= 0 ? n : null;
}

/**
 * Coerce numeric-looking string parameter values to numbers so identity is
 * stable: buildCandidateGrid emits decimal strings while hand-entered params
 * are numbers, and `"0"` vs `0` would otherwise defeat signature dedup.
 */
export function normalizeKernelRef(kernel: KernelRef): KernelRef {
  const parameters: KernelRef["parameters"] = {};
  for (const [key, value] of Object.entries(kernel.parameters)) {
    parameters[key] =
      typeof value === "string" && value.trim() !== "" && !Number.isNaN(Number(value))
        ? Number(value)
        : value;
  }
  return { id: kernel.id, parameters };
}

/** Single Bicubic (b, c) from the add form; null when either input is invalid. */
export function bicubicRefFromDraft(draft: KernelDraft): KernelRef | null {
  const b = parseNonNegativeDecimal(draft.bicubicB);
  const c = parseNonNegativeDecimal(draft.bicubicC);
  if (b === null || c === null) return null;
  return { id: "bicubic", parameters: { b, c } };
}

/** One Lanczos ref per selected taps value, ascending. */
export function lanczosRefsFromDraft(draft: KernelDraft): KernelRef[] {
  return [...new Set(draft.lanczosTapsSelection)]
    .filter((taps) => Number.isInteger(taps) && taps >= 1)
    .sort((a, b) => a - b)
    .map((taps) => ({ id: "lanczos", parameters: { taps } }));
}

/** Lanczos taps chip range from engine metadata (gui_min/gui_max), fallback 2..6. */
export function lanczosTapsRange(capabilities: EngineEnvelope | null): { min: number; max: number } {
  const parameters = capabilities?.payload.kernels.find((kernel) => kernel.id === "lanczos")
    ?.parameters;
  const min = Number(parameters?.["gui_min"]);
  const max = Number(parameters?.["gui_max"]);
  if (Number.isInteger(min) && Number.isInteger(max) && min >= 1 && max >= min && max <= 15) {
    return { min, max };
  }
  return { min: 2, max: 6 };
}

/**
 * Append one kernel to the scan list. Duplicates (same normalized signature)
 * are rejected with `added: false` and the draft is returned unchanged.
 */
export function addKernelToScanList(
  draft: KernelDraft,
  kernel: KernelRef,
): { draft: KernelDraft; added: boolean } {
  const candidate = normalizeKernelRef(kernel);
  const signature = kernelSignature(candidate);
  const seen = new Set(draft.scanList.map((entry) => kernelSignature(normalizeKernelRef(entry))));
  if (seen.has(signature)) return { draft, added: false };
  return { draft: { ...draft, scanList: [...draft.scanList, candidate] }, added: true };
}

/**
 * One-click Bicubic (b, c) sweep: b-outer, c-inner, appended to the scan list
 * with per-entry duplicate skipping.
 */
export function addBicubicGridToScanList(
  draft: KernelDraft,
):
  | { ok: true; draft: KernelDraft; added: number; skipped: number }
  | { ok: false; reason: string } {
  const bGrid = buildCandidateGrid({
    axis: "b",
    start: draft.bStart,
    stop: draft.bStop,
    step: draft.bStep,
    endpointRule: "inclusive",
  });
  if (!bGrid.ok) return bGrid;
  const cGrid = buildCandidateGrid({
    axis: "c",
    start: draft.cStart,
    stop: draft.cStop,
    step: draft.cStep,
    endpointRule: "inclusive",
  });
  if (!cGrid.ok) return cGrid;

  let current = draft;
  let added = 0;
  let skipped = 0;
  for (const b of bGrid.grid.candidates) {
    for (const c of cGrid.grid.candidates) {
      const result = addKernelToScanList(current, { id: "bicubic", parameters: { b, c } });
      current = result.draft;
      if (result.added) added += 1;
      else skipped += 1;
    }
  }
  return { ok: true, draft: current, added, skipped };
}

export function removeKernelFromScanList(draft: KernelDraft, index: number): KernelDraft {
  return { ...draft, scanList: draft.scanList.filter((_, i) => i !== index) };
}

export function clearScanList(draft: KernelDraft): KernelDraft {
  return { ...draft, scanList: [] };
}

/**
 * The scan candidate set is exactly the user's list, filtered to kernels the
 * engine actually reports.
 */
export function resolveKernelCandidates(
  draft: KernelDraft,
  capabilities: EngineEnvelope | null,
): { ok: true; candidates: KernelRef[] } | { ok: false; reason: string } {
  const reported = new Set((capabilities?.payload.kernels ?? []).map((kernel) => kernel.id));
  const candidates = draft.scanList.filter(
    (kernel) => reported.size === 0 || reported.has(kernel.id),
  );
  if (candidates.length === 0) return { ok: false, reason: "no_kernels" };
  return { ok: true, candidates };
}

/** Stable key grouping Samples by the source shape their geometry resolves from. */
export function geometryGroupKey(input: {
  sourceWidth: number;
  sourceHeight: number;
  baseHeight: string;
  baseWidth: string;
  profileId: string;
}): string {
  return [
    input.sourceWidth,
    input.sourceHeight,
    input.baseHeight.trim() || "auto",
    input.baseWidth.trim() || "auto",
    input.profileId,
  ].join("@");
}

export type ResolvedGeometryMap = Record<string, GeometrySnapshot>;

export function estimateKernelWork(input: {
  sampleCount: number;
  candidateCount: number;
}): number {
  return Math.max(0, input.sampleCount) * Math.max(0, input.candidateCount);
}
