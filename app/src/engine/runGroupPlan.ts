import type { EngineEnvelope } from "./types";
import type { CandidateGridSpec, KernelRef, MetricSpec } from "./protocol";
import {
  fixedKernelsForDraft,
  resolveHeightGrid,
  type HeightDraft,
  validateBackendPNorm,
} from "./heightDraft";
import { validateHeightShape } from "./shapeGuards";
import type { HeightAnalyzeRequest } from "./protocol";
import type { ProjectState, Run, RunGroup } from "../project/types";
import type { Translator } from "../i18n";
import { kernelDisplayName } from "./displayNames";

export type HeightRunGroupType =
  | "single_height"
  | "multi_sample_height"
  | "multi_kernel_height"
  | "multi_sample_multi_kernel_height";

export type PlannedHeightMember = {
  /** Stable key within the plan (not a persisted id yet). */
  planKey: string;
  sampleId: string;
  sampleLabel: string;
  sourceId: string;
  sourcePath: string;
  sourceFingerprint?: string | null;
  streamIndex?: number | null;
  frameIndex?: number | null;
  sampleFingerprint?: string | null;
  kernel: KernelRef;
  heightGrid: CandidateGridSpec;
  metric: MetricSpec;
  profileId: string;
  axisMode: HeightDraft["axisMode"];
  backendPreference: HeightDraft["backendPreference"];
  mathMode: HeightDraft["mathMode"];
  /** Engine-valid request body ready for a future worker. */
  request: HeightAnalyzeRequest;
};

export type HeightRunGroupPlan = {
  groupType: HeightRunGroupType;
  label: string;
  memberCount: number;
  candidateCount: number;
  workEstimate: number;
  members: PlannedHeightMember[];
  /** Intent snapshot frozen at plan time; never mutated after commit. */
  intentSnapshot: {
    preset: HeightDraft["preset"];
    axisMode: HeightDraft["axisMode"];
    compareKernels: KernelRef[];
    metric: MetricSpec;
    profileId: string;
    heightGrid: CandidateGridSpec;
    kernels: KernelRef[];
    sampleIds: string[];
    baseHeight: string | null;
    baseWidth: string | null;
  };
};

export type PlanSample = {
  id: string;
  label?: string | null;
  sourceId: string;
  sourceFingerprint?: string | null;
  streamIndex?: number | null;
  frameIndex?: number | null;
  included: boolean;
};

export type PlanSource = {
  id: string;
  path: string;
  fingerprint?: string | null;
  state: string;
  width?: number | null;
  height?: number | null;
};

export function planHeightRunGroup(input: {
  draft: HeightDraft;
  samples: PlanSample[];
  sourcesById: Record<string, PlanSource>;
  capabilities: EngineEnvelope | null;
  nowMs?: number;
  requestIdPrefix?: string;
}): { ok: true; plan: HeightRunGroupPlan } | { ok: false; reason: string } {
  const included = input.samples.filter((sample) => sample.included);
  if (included.length === 0) return { ok: false, reason: "no_samples" };

  const pNorm = validateBackendPNorm(
    input.capabilities,
    input.draft.backendPreference,
    input.draft.metric.pNorm,
    input.draft.axisMode,
  );
  if (!pNorm.ok) return pNorm;

  const grid = resolveHeightGrid(input.draft);
  if (!grid.ok) return grid;

  const kernels = fixedKernelsForDraft(input.draft, input.capabilities);
  if (kernels.length === 0) return { ok: false, reason: "no_kernels" };

  const members: PlannedHeightMember[] = [];
  let requestSeq = 0;
  const prefix = input.requestIdPrefix ?? "req";
  const now = input.nowMs ?? Date.now();
  const baseHeight = input.draft.baseHeight?.trim() || null;
  const baseWidth = input.draft.baseWidth?.trim() || null;
  const baseValid = (value: string | null) => value === null || /^[1-9]\d*$/.test(value);
  if (!baseValid(baseHeight) || !baseValid(baseWidth)) {
    return { ok: false, reason: "base_invalid" };
  }

  for (const sample of included) {
    const source = input.sourcesById[sample.sourceId];
    if (!source || source.state !== "ready") {
      return { ok: false, reason: "sample_source_unavailable" };
    }
    if (
      sample.sourceFingerprint &&
      source.fingerprint &&
      sample.sourceFingerprint !== source.fingerprint
    ) {
      return { ok: false, reason: "sample_fingerprint_stale" };
    }

    for (const kernel of kernels) {
      const requestId = `${prefix}_${now}_${requestSeq++}`;
      const request: HeightAnalyzeRequest = {
        schemaVersion: 1,
        requestId,
        mode: "height",
        sampleId: sample.id,
        sampleFingerprint: sample.sourceFingerprint ?? source.fingerprint ?? null,
        sourcePath: source.path,
        sourceFingerprint: source.fingerprint ?? null,
        streamIndex: sample.streamIndex ?? null,
        frameIndex: sample.frameIndex ?? null,
        kernel,
        axisMode: input.draft.axisMode,
        heightGrid: grid.grid,
        baseHeight,
        baseWidth,
        metric: { ...input.draft.metric },
        profileId: input.draft.profileId,
        mathMode: input.draft.mathMode,
        backendPreference: input.draft.backendPreference,
      };
      const shape = validateHeightShape(request);
      if (!shape.ok) return { ok: false, reason: shape.code };

      members.push({
        planKey: `${sample.id}::${kernel.id}::${JSON.stringify(kernel.parameters)}`,
        sampleId: sample.id,
        sampleLabel: sample.label ?? sample.id,
        sourceId: source.id,
        sourcePath: source.path,
        sourceFingerprint: source.fingerprint ?? null,
        streamIndex: sample.streamIndex ?? null,
        frameIndex: sample.frameIndex ?? null,
        sampleFingerprint: sample.sourceFingerprint ?? source.fingerprint ?? null,
        kernel,
        heightGrid: grid.grid,
        metric: { ...input.draft.metric },
        profileId: input.draft.profileId,
        axisMode: input.draft.axisMode,
        backendPreference: input.draft.backendPreference,
        mathMode: input.draft.mathMode,
        request,
      });
    }
  }

  const multiSample = included.length > 1;
  const multiKernel = kernels.length > 1;
  const groupType: HeightRunGroupType =
    multiSample && multiKernel
      ? "multi_sample_multi_kernel_height"
      : multiSample
        ? "multi_sample_height"
        : multiKernel
          ? "multi_kernel_height"
          : "single_height";

  const candidateCount = grid.grid.candidates.length;
  const workEstimate = members.length * candidateCount;

  return {
    ok: true,
    plan: {
      groupType,
      label: multiKernel
        ? "Compare Kernels / Resolution Test"
        : multiSample
          ? "Multi-Sample Resolution Test"
          : "Resolution Test",
      memberCount: members.length,
      candidateCount,
      workEstimate,
      members,
      intentSnapshot: {
        preset: input.draft.preset,
        axisMode: input.draft.axisMode,
        compareKernels: input.draft.compareKernels.map((kernel) => ({
          id: kernel.id,
          parameters: { ...kernel.parameters },
        })),
        metric: { ...input.draft.metric },
        profileId: input.draft.profileId,
        heightGrid: grid.grid,
        kernels,
        sampleIds: included.map((sample) => sample.id),
        baseHeight: input.draft.baseHeight?.trim() || null,
        baseWidth: input.draft.baseWidth?.trim() || null,
      },
    },
  };
}

/**
 * Materialize a plan into immutable Project Run/RunGroup records.
 * Results are always null here; only a real worker may fill them later.
 */
export function materializeHeightRunGroup(input: {
  plan: HeightRunGroupPlan;
  idFactory?: () => string;
  nowIso?: string;
}): { runGroup: RunGroup; runs: Run[] } {
  const makeId = input.idFactory ?? (() => crypto.randomUUID());
  const nowIso = input.nowIso ?? new Date().toISOString();
  const groupId = `rgrp_${makeId()}`;
  const runs = input.plan.members.map((member) => {
    const runId = `run_${makeId()}`;
    return {
      id: runId,
      runType: "height",
      status: "queued",
      runGroupId: groupId,
      sampleId: member.sampleId,
      sourceId: member.sourceId,
      createdAt: nowIso,
      updatedAt: nowIso,
      inputSnapshot: {
        planKey: member.planKey,
        request: member.request,
        kernel: member.kernel,
        heightGrid: member.heightGrid,
        metric: member.metric,
        profileId: member.profileId,
      },
      result: null,
      errorCode: null,
      errorMessage: null,
      completed: 0,
      total: member.heightGrid.candidates.length,
    };
  });

  return {
    runGroup: {
      id: groupId,
      groupType: input.plan.groupType,
      label: input.plan.label,
      memberRunIds: runs.map((run) => run.id),
      createdAt: nowIso,
      intentSnapshot: input.plan.intentSnapshot,
    },
    runs,
  };
}

/** MetricSpec compatibility key: incompatible metrics must not overlay. */
export function metricCompatibilityKey(metric: MetricSpec): string {
  return [
    metric.cropLeft,
    metric.cropRight,
    metric.cropTop,
    metric.cropBottom,
    metric.pixelExclusionThreshold,
    metric.pNorm,
  ].join("|");
}

export type HeightSeriesPoint = {
  height: string;
  metric: number;
};

/** Extract height series only from real engine-shaped results; never invent values. */
export function extractHeightSeries(result: unknown): HeightSeriesPoint[] | null {
  if (!result || typeof result !== "object") return null;
  const record = result as Record<string, unknown>;
  // Worker protocol v1 height payload: {candidates: [{id, error}], telemetry}.
  const wireCandidates = record.candidates;
  if (Array.isArray(wireCandidates)) {
    const points: HeightSeriesPoint[] = [];
    for (const item of wireCandidates) {
      if (!item || typeof item !== "object") continue;
      const row = item as Record<string, unknown>;
      const id = row.id;
      const rawError = row.error;
      if (id == null || rawError == null) continue;
      const metric = typeof rawError === "number" ? rawError : Number(rawError);
      if (!Number.isFinite(metric)) continue;
      points.push({ height: String(id), metric });
    }
    return points.length ? points : null;
  }
  const series = record.series ?? record.points ?? record.metrics;
  if (!Array.isArray(series)) return null;
  const points: HeightSeriesPoint[] = [];
  for (const item of series) {
    if (!item || typeof item !== "object") continue;
    const row = item as Record<string, unknown>;
    const height = row.height ?? row.candidate ?? row.x;
    const metric = row.metric ?? row.value ?? row.y ?? row.error;
    if (height == null || metric == null) continue;
    const metricNumber = typeof metric === "number" ? metric : Number(metric);
    if (!Number.isFinite(metricNumber)) continue;
    points.push({ height: String(height), metric: metricNumber });
  }
  return points.length ? points : null;
}

export type SeriesTableRow = {
  runId: string;
  height: string;
  metric: number;
  sampleLabel: string;
  kernelId: string;
  /** Kernel identity including parameter variant, e.g. "lanczos@3". */
  kernelKey: string;
};

/** Per-run series metadata (one entry per run contributing plot points). */
export type SeriesMeta = {
  runId: string;
  sampleId: string;
  sampleLabel: string;
  kernelId: string;
  kernelTaps: number | null;
  kernelKey: string;
};

export type SeriesTable = {
  rows: SeriesTableRow[];
  incompatibleCount: number;
  seriesMeta: SeriesMeta[];
};

export function buildSeriesTable(
  runs: Run[],
  state: ProjectState,
  hiddenSampleIds: Set<string>,
  activeMetricKey: string,
): SeriesTable {
  const rows: SeriesTableRow[] = [];
  const seriesMeta: SeriesMeta[] = [];
  let incompatibleCount = 0;
  for (const run of runs) {
    if (run.sampleId && hiddenSampleIds.has(run.sampleId)) continue;
    const snapshot = run.inputSnapshot as
      | { metric?: { cropLeft: number; cropRight: number; cropTop: number; cropBottom: number; pixelExclusionThreshold: number; pNorm: number }; kernel?: { id: string; parameters?: Record<string, string | number | boolean> } }
      | null;
    if (snapshot?.metric) {
      if (metricCompatibilityKey(snapshot.metric) !== activeMetricKey) {
        incompatibleCount += 1;
        continue;
      }
    }
    const series = extractHeightSeries(run.result);
    if (!series) continue;
    const sample = run.sampleId ? state.samplesById[run.sampleId] : null;
    const kernelId = snapshot?.kernel?.id ?? "—";
    const rawTaps = Number(snapshot?.kernel?.parameters?.taps);
    const kernelTaps = Number.isFinite(rawTaps) ? rawTaps : null;
    const kernelKey = kernelTaps != null ? `${kernelId}@${kernelTaps}` : kernelId;
    const sampleLabel = sample?.label ?? run.sampleId ?? "—";
    seriesMeta.push({
      runId: run.id,
      sampleId: run.sampleId ?? "",
      sampleLabel,
      kernelId,
      kernelTaps,
      kernelKey,
    });
    for (const point of series) {
      rows.push({
        runId: run.id,
        height: point.height,
        metric: point.metric,
        sampleLabel,
        kernelId,
        kernelKey,
      });
    }
  }
  return { rows, incompatibleCount, seriesMeta };
}

export function kernelMetaLabel(
  t: Translator,
  meta: { kernelId: string; kernelTaps: number | null },
): string {
  const name = kernelDisplayName(t, meta.kernelId);
  return meta.kernelTaps != null ? `${name} ${meta.kernelTaps}` : name;
}
