import type { BackendPreference, MetricSpec, ScanScope, VerifyRequest } from "./protocol";
import { validateVerifyShape } from "./shapeGuards";
import { recipeReadiness } from "../project/recipe";
import type { ProjectState, Recipe, Run, RunGroup, Source } from "../project/types";
import { geometryForSource } from "./geometry";
import { metricCompatibilityKey } from "./runGroupPlan";

/**
 * Whole-video Verification (全视频检查) setup. One member VerificationRun per
 * selected Source; every Run snapshots the current Recipe geometry/kernel.
 * MetricSpec may be overridden on the draft (inherit from Resolution Test or
 * edited locally) without rewriting the Recipe.
 */

export type VerifyScopeKind = "full" | "preview";

export type VerifyDraft = {
  /** Selected ready video Sources; one member VerificationRun each. */
  sourceIds: string[];
  scopeKind: VerifyScopeKind;
  /** Preview Scan only: decoded I-pictures or every N frames. */
  previewRule: "decoded_i_picture" | "every_n";
  everyN: string;
  /** Empty string = whole Source. */
  startFrame: string;
  endFrame: string;
  backendPreference: BackendPreference;
  concurrency: number;
  /** When set, the scan uses this MetricSpec instead of `recipe.metric`. */
  metric?: MetricSpec;
};

export function defaultVerifyDraft(backendPreference: BackendPreference = "auto"): VerifyDraft {
  return {
    sourceIds: [],
    // Preview (every decoded I-picture) is the default; full scans are opt-in.
    scopeKind: "preview",
    previewRule: "decoded_i_picture",
    everyN: "24",
    startFrame: "",
    endFrame: "",
    backendPreference,
    concurrency: 8,
  };
}

/** Keep the draft selection valid as the set of indexed video sources changes. */
export function reconcileReadyVideoSourceIds(
  selectedSourceIds: string[],
  readyVideoSourceIds: string[],
): string[] {
  const ready = new Set(readyVideoSourceIds);
  const retained = selectedSourceIds.filter((sourceId) => ready.has(sourceId));
  const next =
    retained.length === 0 && readyVideoSourceIds.length === 1
      ? [readyVideoSourceIds[0]]
      : retained;

  return next.length === selectedSourceIds.length &&
    next.every((sourceId, index) => sourceId === selectedSourceIds[index])
    ? selectedSourceIds
    : next;
}

export type VerifyRunGroupPlan = {
  groupType: "multi_source_verification" | "single_verification";
  label: string;
  memberCount: number;
  members: Array<{
    planKey: string;
    sourceId: string;
    sourceLabel: string;
    scanScope: ScanScope;
    request: VerifyRequest;
  }>;
  intentSnapshot: {
    recipeId: string;
    recipeRevision: number;
    sourceIds: string[];
    scopeKind: VerifyScopeKind;
    concurrency: number;
  };
};

function parseFrameNumber(value: string): number | null | "invalid" {
  const trimmed = value.trim();
  if (!trimmed) return null;
  if (!/^\d+$/.test(trimmed)) return "invalid";
  const n = Number(trimmed);
  return Number.isInteger(n) && n >= 0 ? n : "invalid";
}

export function resolveScanScope(
  draft: VerifyDraft,
  streamIndex: number,
): { ok: true; scope: ScanScope } | { ok: false; reason: string } {
  const startFrame = parseFrameNumber(draft.startFrame);
  const endFrame = parseFrameNumber(draft.endFrame);
  if (startFrame === "invalid" || endFrame === "invalid") {
    return { ok: false, reason: "verify_range_invalid" };
  }
  if (startFrame != null && endFrame != null && startFrame > endFrame) {
    return { ok: false, reason: "verify_range_invalid" };
  }
  if (draft.scopeKind === "full") {
    return {
      ok: true,
      scope: { streamIndex, selection: "all", startFrame, endFrame },
    };
  }
  if (draft.previewRule === "every_n") {
    const everyN = Number(draft.everyN.trim());
    if (!Number.isInteger(everyN) || everyN < 1) {
      return { ok: false, reason: "verify_every_n_invalid" };
    }
    return {
      ok: true,
      scope: { streamIndex, selection: "every_n", everyN, startFrame, endFrame },
    };
  }
  return {
    ok: true,
    scope: { streamIndex, selection: "decoded_i_picture", startFrame, endFrame },
  };
}

export const GPU_VERIFY_CONCURRENCY_MAX = 8;

export function verifyConcurrencyMaximum(
  resolvedBackend: string,
  capabilityMax = 16,
  gpuMax = GPU_VERIFY_CONCURRENCY_MAX,
): number {
  if (resolvedBackend === "cpu") return capabilityMax;
  return Math.min(capabilityMax, gpuMax);
}

export function validVerifyConcurrency(value: number, maximum = 16): boolean {
  return Number.isInteger(value) && value >= 1 && value <= maximum;
}

export function planVerifyRunGroup(input: {
  draft: VerifyDraft;
  recipe: Recipe;
  sourcesById: Record<string, Source>;
  nowMs?: number;
  requestIdPrefix?: string;
}): { ok: true; plan: VerifyRunGroupPlan } | { ok: false; reason: string } {
  const readiness = recipeReadiness(input.recipe);
  if (!readiness.ok) return { ok: false, reason: "recipe_incomplete" };
  // recipeReadiness guarantees geometry/kernel/metric are present.
  const recipeGeometry = input.recipe.geometry!;
  const recipeKernel = input.recipe.kernel!;
  const recipeMetric = input.draft.metric ?? input.recipe.metric!;
  const selected = input.draft.sourceIds
    .map((id) => input.sourcesById[id])
    .filter((source): source is Source => Boolean(source));
  if (selected.length === 0) return { ok: false, reason: "no_sources" };
  if (!validVerifyConcurrency(input.draft.concurrency)) {
    return { ok: false, reason: "verify_concurrency_invalid" };
  }

  const prefix = input.requestIdPrefix ?? "req";
  const now = input.nowMs ?? Date.now();
  const members: VerifyRunGroupPlan["members"] = [];

  for (const [index, source] of selected.entries()) {
    if (source.kind !== "video") return { ok: false, reason: "source_not_video" };
    if (source.state !== "ready") return { ok: false, reason: "source_unavailable" };
    if (!source.width || !source.height) return { ok: false, reason: "source_dims_missing" };
    const streamIndex = source.selectedStreamIndex ?? 0;
    const scope = resolveScanScope(input.draft, streamIndex);
    if (!scope.ok) return scope;
    const request: VerifyRequest = {
      schemaVersion: 1,
      requestId: `${prefix}_v_${now}_${index}`,
      mode: "verify",
      sourceId: source.id,
      sourcePath: source.path,
      sourceFingerprint: source.fingerprint ?? null,
      recipeId: input.recipe.id,
      recipeRevision: input.recipe.revision,
      geometry: geometryForSource(
        recipeGeometry,
        input.recipe.axisMode,
        source.width,
        source.height,
      ),
      kernel: recipeKernel,
      metric: recipeMetric,
      axisMode: input.recipe.axisMode,
      profileId: input.recipe.profileId ?? "",
      mathMode: input.recipe.mathMode ?? "raw",
      scanScope: scope.scope,
      backendPreference: input.draft.backendPreference,
      concurrency: input.draft.concurrency,
    };
    const shape = validateVerifyShape(request);
    if (!shape.ok) return { ok: false, reason: shape.code };
    members.push({
      planKey: `${source.id}::${input.recipe.id}@${input.recipe.revision}::${metricCompatibilityKey(recipeMetric)}`,
      sourceId: source.id,
      sourceLabel: source.label || source.path,
      scanScope: scope.scope,
      request,
    });
  }

  return {
    ok: true,
    plan: {
      groupType: selected.length > 1 ? "multi_source_verification" : "single_verification",
      label:
        input.draft.scopeKind === "full" ? "Full Video Check" : "Preview Scan",
      memberCount: members.length,
      members,
      intentSnapshot: {
        recipeId: input.recipe.id,
        recipeRevision: input.recipe.revision,
        sourceIds: selected.map((source) => source.id),
        scopeKind: input.draft.scopeKind,
        concurrency: input.draft.concurrency,
      },
    },
  };
}

/** Materialize a verification plan into immutable Project Run/RunGroup records. */
export function materializeVerifyRunGroup(input: {
  plan: VerifyRunGroupPlan;
  idFactory?: () => string;
  nowIso?: string;
}): { runGroup: RunGroup; runs: Run[] } {
  const makeId = input.idFactory ?? (() => crypto.randomUUID());
  const nowIso = input.nowIso ?? new Date().toISOString();
  const groupId = `rgrp_${makeId()}`;
  const runs = input.plan.members.map((member) => ({
    id: `run_${makeId()}`,
    runType: "verification",
    status: "queued",
    runGroupId: groupId,
    sampleId: null,
    sourceId: member.sourceId,
    createdAt: nowIso,
    updatedAt: nowIso,
    inputSnapshot: {
      planKey: member.planKey,
      request: member.request,
      scanScope: member.scanScope,
      concurrency: member.request.concurrency,
    },
    result: null,
    errorCode: null,
    errorMessage: null,
    completed: 0,
    total: 0,
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

export type VerifyFrameRow = {
  frameIndex: number;
  metric: number;
};

/** Extract per-frame metrics only from real engine-shaped results; never invent values. */
export function extractVerifyFrames(result: unknown): VerifyFrameRow[] | null {
  if (!result || typeof result !== "object") return null;
  const record = result as Record<string, unknown>;
  const frames = record.frames ?? record.rows ?? record.metrics;
  if (!Array.isArray(frames)) return null;
  const rows: VerifyFrameRow[] = [];
  for (const item of frames) {
    if (!item || typeof item !== "object") continue;
    const row = item as Record<string, unknown>;
    const frame = row.frame ?? row.frameIndex ?? row.index;
    const rawMetric = row.metric ?? row.value ?? row.error;
    if (frame == null || rawMetric == null) continue;
    const frameIndex = Number(frame);
    const metric = typeof rawMetric === "number" ? rawMetric : Number(rawMetric);
    if (!Number.isInteger(frameIndex) || !Number.isFinite(metric)) continue;
    rows.push({ frameIndex, metric });
  }
  return rows.length ? rows : null;
}

export function verificationRuns(state: ProjectState) {
  return Object.values(state.runsById)
    .filter((run) => run.runType === "verification" || run.runType === "verify")
    .sort((a, b) => b.createdAt.localeCompare(a.createdAt));
}
