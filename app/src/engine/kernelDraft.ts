import type { EngineEnvelope } from "./types";
import type {
  BackendPreference,
  GeometrySnapshot,
  KernelRef,
  MathMode,
  MetricSpec,
} from "./protocol";
import { buildCandidateGrid } from "./candidateGrid";

/**
 * Kernel Analysis (算法测试) draft: one fixed geometry per Sample source shape,
 * many kernel candidates. Geometry is never edited here — it is resolved from
 * the engine `geometry` command and displayed read-only.
 */

export type KernelScanMode = "preset_families" | "bicubic_grid";

/** Fixed deterministic family order; the exact candidate sequence is user-visible. */
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
  scanMode: KernelScanMode;
  families: Record<KernelFamilyId, boolean>;
  /** Single taps value used by the Lanczos preset family. */
  lanczosTaps: string;
  /** (b, c) used by the Bicubic preset family. */
  bicubicB: string;
  bicubicC: string;
  /** Bicubic Grid mode: b and c sweep ranges (decimal strings). */
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
): KernelDraft {
  void capabilities;
  return {
    scanMode: "preset_families",
    families: {
      bilinear: true,
      bicubic: true,
      spline16: true,
      spline36: true,
      spline64: true,
      lanczos: true,
    },
    lanczosTaps: "3",
    bicubicB: "0",
    bicubicC: "0.5",
    bStart: "0",
    bStop: "1",
    bStep: "0.2",
    cStart: "0",
    cStop: "1",
    cStep: "0.2",
    baseHeight: "720",
    baseWidth: "",
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

function parsePositiveInteger(value: string): number | null {
  const trimmed = value.trim();
  if (!/^\d+$/.test(trimmed)) return null;
  const n = Number(trimmed);
  return Number.isInteger(n) && n >= 1 ? n : null;
}

/**
 * Resolve the exact ordered kernel candidate sequence for the draft.
 * Preset families emit one representative per selected family; Bicubic Grid
 * emits the full (b, c) sweep ordered b-outer, c-inner. Candidates are
 * filtered to kernels the engine actually reports.
 */
export function resolveKernelCandidates(
  draft: KernelDraft,
  capabilities: EngineEnvelope | null,
): { ok: true; candidates: KernelRef[] } | { ok: false; reason: string } {
  const reported = new Set((capabilities?.payload.kernels ?? []).map((kernel) => kernel.id));
  const available = (id: string) => reported.size === 0 || reported.has(id);

  if (draft.scanMode === "preset_families") {
    const bicubicB = parseNonNegativeDecimal(draft.bicubicB);
    const bicubicC = parseNonNegativeDecimal(draft.bicubicC);
    if (draft.families.bicubic && (bicubicB === null || bicubicC === null)) {
      return { ok: false, reason: "bicubic_params_invalid" };
    }
    const taps = parsePositiveInteger(draft.lanczosTaps);
    if (draft.families.lanczos && taps === null) {
      return { ok: false, reason: "lanczos_taps_invalid" };
    }
    const candidates: KernelRef[] = [];
    for (const family of KERNEL_FAMILY_ORDER) {
      if (!draft.families[family] || !available(family)) continue;
      if (family === "bicubic") {
        candidates.push({ id: "bicubic", parameters: { b: bicubicB ?? 0, c: bicubicC ?? 0 } });
      } else if (family === "lanczos") {
        candidates.push({ id: "lanczos", parameters: { taps: taps ?? 3 } });
      } else {
        candidates.push({ id: family, parameters: {} });
      }
    }
    if (candidates.length === 0) return { ok: false, reason: "no_kernels" };
    return { ok: true, candidates };
  }

  if (!available("bicubic")) return { ok: false, reason: "no_kernels" };
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
  const candidates: KernelRef[] = [];
  for (const b of bGrid.grid.candidates) {
    for (const c of cGrid.grid.candidates) {
      candidates.push({ id: "bicubic", parameters: { b, c } });
    }
  }
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
