import type { EngineEnvelope } from "./types";
import type { AxisMode, KernelRef } from "./protocol";
import { validateBackendPNorm } from "./heightDraft";
import {
  geometryGroupKey,
  resolveKernelCandidates,
  type KernelDraft,
  type ResolvedGeometryMap,
} from "./kernelDraft";
import { validateKernelShape } from "./shapeGuards";
import type { KernelAnalyzeRequest, GeometrySnapshot, MetricSpec } from "./protocol";
import { metricCompatibilityKey, type PlanSample, type PlanSource } from "./runGroupPlan";
import type { ProjectState, Run, RunGroup } from "../project/types";

export type KernelRunGroupType = "single_kernel" | "multi_sample_kernel";

export type PlannedKernelMember = {
  planKey: string;
  sampleId: string;
  sampleLabel: string;
  sourceId: string;
  sourcePath: string;
  geometry: GeometrySnapshot;
  kernels: KernelRef[];
  /** Engine-valid request body ready for a future worker. */
  request: KernelAnalyzeRequest;
};

export type KernelRunGroupPlan = {
  groupType: KernelRunGroupType;
  label: string;
  memberCount: number;
  kernelCount: number;
  workEstimate: number;
  members: PlannedKernelMember[];
  intentSnapshot: {
    kernels: KernelRef[];
    metric: KernelDraft["metric"];
    profileId: string;
    axisMode: AxisMode;
    sampleIds: string[];
    geometryKeys: string[];
  };
};

/**
 * Plan a Kernel Analysis RunGroup: one member KernelRun per included Sample,
 * each with one fixed geometry (resolved from the engine geometry command)
 * and the full ordered kernel candidate list.
 */
export function planKernelRunGroup(input: {
  draft: KernelDraft;
  samples: PlanSample[];
  sourcesById: Record<string, PlanSource>;
  geometries: ResolvedGeometryMap;
  capabilities: EngineEnvelope | null;
  axisMode: AxisMode;
  nowMs?: number;
  requestIdPrefix?: string;
}): { ok: true; plan: KernelRunGroupPlan } | { ok: false; reason: string } {
  const included = input.samples.filter((sample) => sample.included);
  if (included.length === 0) return { ok: false, reason: "no_samples" };
  const pNorm = validateBackendPNorm(
    input.capabilities,
    input.draft.backendPreference,
    input.draft.metric.pNorm,
    input.axisMode,
  );
  if (!pNorm.ok) return pNorm;

  const resolved = resolveKernelCandidates(input.draft, input.capabilities);
  if (!resolved.ok) return resolved;
  if (resolved.candidates.length < 2) return { ok: false, reason: "kernel_list_too_small" };

  const members: PlannedKernelMember[] = [];
  const geometryKeys = new Set<string>();
  let requestSeq = 0;
  const prefix = input.requestIdPrefix ?? "req";
  const now = input.nowMs ?? Date.now();

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
    const width = source.width;
    const height = source.height;
    if (!width || !height) return { ok: false, reason: "source_dims_missing" };

    const key = geometryGroupKey({
      sourceWidth: width,
      sourceHeight: height,
      baseHeight: input.draft.baseHeight,
      baseWidth: input.draft.baseWidth,
      profileId: input.draft.profileId,
    });
    const geometry = input.geometries[key];
    if (!geometry) return { ok: false, reason: "geometry_unresolved" };
    geometryKeys.add(key);

    const requestId = `${prefix}_k_${now}_${requestSeq++}`;
    const request: KernelAnalyzeRequest = {
      schemaVersion: 1,
      requestId,
      mode: "kernel",
      sampleId: sample.id,
      sampleFingerprint: sample.sourceFingerprint ?? source.fingerprint ?? null,
      sourcePath: source.path,
      sourceFingerprint: source.fingerprint ?? null,
      streamIndex: sample.streamIndex ?? null,
      frameIndex: sample.frameIndex ?? null,
      geometry,
      axisMode: input.axisMode,
      kernels: resolved.candidates.map((kernel) => ({
        id: kernel.id,
        parameters: { ...kernel.parameters },
      })),
      metric: { ...input.draft.metric },
      profileId: input.draft.profileId,
      mathMode: input.draft.mathMode,
      backendPreference: input.draft.backendPreference,
    };
    const shape = validateKernelShape(request);
    if (!shape.ok) return { ok: false, reason: shape.code };

    members.push({
      planKey: `${sample.id}::${key}`,
      sampleId: sample.id,
      sampleLabel: sample.label ?? sample.id,
      sourceId: source.id,
      sourcePath: source.path,
      geometry,
      kernels: resolved.candidates,
      request,
    });
  }

  return {
    ok: true,
    plan: {
      groupType: included.length > 1 ? "multi_sample_kernel" : "single_kernel",
      label: included.length > 1 ? "Multi-Sample Algorithm Test" : "Algorithm Test",
      memberCount: members.length,
      kernelCount: resolved.candidates.length,
      workEstimate: members.length * resolved.candidates.length,
      members,
      intentSnapshot: {
        kernels: resolved.candidates,
        metric: { ...input.draft.metric },
        profileId: input.draft.profileId,
        axisMode: input.axisMode,
        sampleIds: included.map((sample) => sample.id),
        geometryKeys: [...geometryKeys],
      },
    },
  };
}

export type KernelResultEntry = {
  kernelId: string;
  parameters: Record<string, unknown>;
  metric: number;
};

/** Extract per-kernel metrics only from real engine-shaped results; never invent values. */
export function extractKernelResultRows(result: unknown): KernelResultEntry[] | null {
  if (!result || typeof result !== "object") return null;
  const record = result as Record<string, unknown>;
  // Worker protocol v1.1 kernel payload: {candidates: [{id: "<index>", error,
  // kernel: {...echo...}}], candidate, telemetry}; tolerate row/result shapes.
  const rows = record.candidates ?? record.rows ?? record.results ?? record.metrics;
  if (!Array.isArray(rows)) return null;
  const extracted: KernelResultEntry[] = [];
  for (const item of rows) {
    if (!item || typeof item !== "object") continue;
    const row = item as Record<string, unknown>;
    const kernel = row.kernel as Record<string, unknown> | undefined;
    const kernelId = (kernel?.id ?? row.kernelId) as string | undefined;
    const rawMetric = row.error ?? row.metric ?? row.value;
    if (!kernelId || rawMetric == null) continue;
    const metric = typeof rawMetric === "number" ? rawMetric : Number(rawMetric);
    if (!Number.isFinite(metric)) continue;
    const { id: _echoId, ...echoParams } = kernel ?? {};
    extracted.push({
      kernelId,
      parameters:
        kernel != null
          ? echoParams
          : ((row.parameters ?? {}) as Record<string, unknown>),
      metric,
    });
  }
  return extracted.length ? extracted : null;
}

export type KernelResultRow = {
  runId: string;
  sampleId: string;
  kernelId: string;
  parameters: KernelRef["parameters"];
  kernelLabel: string;
  metric: number;
  sampleLabel: string;
};

const KERNEL_SORT_ORDER = new Map(
  ["bilinear", "bicubic", "lanczos", "spline16", "spline36", "spline64"]
    .map((id, index) => [id, index]),
);

function compareKernelParameterValue(a: unknown, b: unknown): number {
  const aNumber = typeof a === "number" ? a : Number(a);
  const bNumber = typeof b === "number" ? b : Number(b);
  if (Number.isFinite(aNumber) && Number.isFinite(bNumber)) return aNumber - bNumber;
  return String(a).localeCompare(String(b), undefined, { numeric: true, sensitivity: "base" });
}

/** Canonical display order: kernel family, parameter name/value, sample, then Run. */
export function compareKernelResultRows(a: KernelResultRow, b: KernelResultRow): number {
  const aRank = KERNEL_SORT_ORDER.get(a.kernelId) ?? Number.MAX_SAFE_INTEGER;
  const bRank = KERNEL_SORT_ORDER.get(b.kernelId) ?? Number.MAX_SAFE_INTEGER;
  if (aRank !== bRank) return aRank - bRank;
  const idOrder = a.kernelId.localeCompare(b.kernelId, undefined, {
    numeric: true,
    sensitivity: "base",
  });
  if (idOrder !== 0) return idOrder;

  const aParameters = Object.entries(a.parameters).sort(([aKey], [bKey]) =>
    aKey.localeCompare(bKey, undefined, { numeric: true, sensitivity: "base" })
  );
  const bParameters = Object.entries(b.parameters).sort(([aKey], [bKey]) =>
    aKey.localeCompare(bKey, undefined, { numeric: true, sensitivity: "base" })
  );
  for (let index = 0; index < Math.max(aParameters.length, bParameters.length); index += 1) {
    const aEntry = aParameters[index];
    const bEntry = bParameters[index];
    if (!aEntry) return -1;
    if (!bEntry) return 1;
    const keyOrder = aEntry[0].localeCompare(bEntry[0], undefined, {
      numeric: true,
      sensitivity: "base",
    });
    if (keyOrder !== 0) return keyOrder;
    const valueOrder = compareKernelParameterValue(aEntry[1], bEntry[1]);
    if (valueOrder !== 0) return valueOrder;
  }
  const sampleOrder = a.sampleLabel.localeCompare(b.sampleLabel, undefined, {
    numeric: true,
    sensitivity: "base",
  });
  return sampleOrder !== 0 ? sampleOrder : a.runId.localeCompare(b.runId);
}

/**
 * Flatten kernel runs into result-table rows, hiding runs whose snapshot
 * metric is incompatible with the currently drafted metric.
 */
export function buildKernelResultRows(
  runs: Run[],
  state: ProjectState,
  activeMetricKey: string,
): { rows: KernelResultRow[]; incompatibleCount: number } {
  const rows: KernelResultRow[] = [];
  let incompatibleCount = 0;
  for (const run of runs) {
    const snapshot = run.inputSnapshot as {
      metric?: MetricSpec;
    } | null;
    if (snapshot?.metric && metricCompatibilityKey(snapshot.metric) !== activeMetricKey) {
      incompatibleCount += 1;
      continue;
    }
    const extracted = extractKernelResultRows(run.result);
    if (!extracted) continue;
    const sample = run.sampleId ? state.samplesById[run.sampleId] : null;
    for (const row of extracted) {
      const params = Object.entries(row.parameters);
      rows.push({
        runId: run.id,
        sampleId: run.sampleId ?? "",
        kernelId: row.kernelId,
        // Engine echoes the parameters we sent (string | number | boolean).
        parameters: { ...row.parameters } as KernelRef["parameters"],
        kernelLabel: params.length
          ? `${row.kernelId} (${params.map(([key, value]) => `${key}=${value}`).join(", ")})`
          : row.kernelId,
        metric: row.metric,
        sampleLabel: sample?.label ?? run.sampleId ?? "—",
      });
    }
  }
  return { rows, incompatibleCount };
}

/** Materialize a kernel plan into immutable Project Run/RunGroup records. */
export function materializeKernelRunGroup(input: {
  plan: KernelRunGroupPlan;
  idFactory?: () => string;
  nowIso?: string;
}): { runGroup: RunGroup; runs: Run[] } {
  const makeId = input.idFactory ?? (() => crypto.randomUUID());
  const nowIso = input.nowIso ?? new Date().toISOString();
  const groupId = `rgrp_${makeId()}`;
  const runs = input.plan.members.map((member) => ({
    id: `run_${makeId()}`,
    runType: "kernel",
    status: "queued",
    runGroupId: groupId,
    sampleId: member.sampleId,
    sourceId: member.sourceId,
    createdAt: nowIso,
    updatedAt: nowIso,
    inputSnapshot: {
      planKey: member.planKey,
      request: member.request,
      kernels: member.kernels,
      geometry: member.geometry,
      metric: member.request.metric,
      profileId: member.request.profileId,
    },
    result: null,
    errorCode: null,
    errorMessage: null,
    completed: 0,
    total: member.kernels.length,
  }));
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
