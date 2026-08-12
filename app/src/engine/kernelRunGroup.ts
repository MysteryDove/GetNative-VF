import type { EngineEnvelope } from "./types";
import type { KernelRef } from "./protocol";
import { profileFor } from "./profiles";
import { validateBackendPNorm } from "./heightDraft";
import {
  geometryGroupKey,
  resolveKernelCandidates,
  type KernelDraft,
  type ResolvedGeometryMap,
} from "./kernelDraft";
import { validateKernelShape } from "./shapeGuards";
import type { KernelAnalyzeRequest, GeometrySnapshot } from "./protocol";
import type { PlanSample, PlanSource } from "./runGroupPlan";

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
  nowMs?: number;
  requestIdPrefix?: string;
}): { ok: true; plan: KernelRunGroupPlan } | { ok: false; reason: string } {
  const included = input.samples.filter((sample) => sample.included);
  if (included.length === 0) return { ok: false, reason: "no_samples" };
  const pNorm = validateBackendPNorm(
    input.capabilities,
    input.draft.backendPreference,
    input.draft.metric.pNorm,
    profileFor(input.draft.profileId, input.capabilities).default_axis_mode,
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
      axisMode: profileFor(input.draft.profileId, input.capabilities).default_axis_mode,
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

/** Materialize a kernel plan into immutable Project Run/RunGroup records. */
export function materializeKernelRunGroup(input: {
  plan: KernelRunGroupPlan;
  idFactory?: () => string;
  nowIso?: string;
}): {
  runGroup: {
    id: string;
    groupType: string;
    label: string;
    memberRunIds: string[];
    createdAt: string;
    intentSnapshot: unknown;
  };
  runs: Array<{
    id: string;
    runType: string;
    status: string;
    runGroupId: string;
    sampleId: string | null;
    sourceId: string | null;
    createdAt: string;
    updatedAt: string;
    inputSnapshot: unknown;
    result: null;
    errorCode: null;
    errorMessage: null;
    completed: number;
    total: number;
  }>;
} {
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
