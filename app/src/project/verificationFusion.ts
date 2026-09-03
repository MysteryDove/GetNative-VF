import type { AxisMode, MathMode, MetricSpec } from "../engine/protocol";
import { validateMetricSpec } from "../engine/shapeGuards";
import { storedVerifyCoverage, storedVerifyFrames } from "../engine/verifyResults";
import type {
  ProjectState,
  Recipe,
  Run,
  Source,
  VerificationFusion,
  VerificationFusionCandidate,
  VerificationFusionFrame,
  VerificationFusionInput,
} from "./types";

type SnapshotRecord = Record<string, unknown>;

export function snakeCaseSnapshot(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(snakeCaseSnapshot);
  if (!value || typeof value !== "object") return value;
  return Object.entries(value as Record<string, unknown>).reduce<Record<string, unknown>>((result, [key, nested]) => {
    result[key.replace(/[A-Z]/g, (letter) => `_${letter.toLowerCase()}`)] = snakeCaseSnapshot(nested);
    return result;
  }, {});
}

function record(value: unknown): SnapshotRecord | null {
  return value && typeof value === "object" && !Array.isArray(value)
    ? value as SnapshotRecord
    : null;
}

function requestOf(run: Run): SnapshotRecord | null {
  const snapshot = record(run.inputSnapshot);
  const request = record(snapshot?.request);
  return request && snapshot ? { ...snapshot, ...request } : request ?? snapshot;
}

function validMetric(value: unknown): value is MetricSpec {
  const metric = record(value);
  if (!metric) return false;
  return ["cropLeft", "cropRight", "cropTop", "cropBottom", "pixelExclusionThreshold", "pNorm"]
    .every((key) => typeof metric[key] === "number" && Number.isFinite(metric[key] as number))
    && validateMetricSpec(metric as unknown as MetricSpec).ok;
}

function metricDifference(left: MetricSpec, right: MetricSpec): string | null {
  if (left.cropLeft !== right.cropLeft || left.cropRight !== right.cropRight || left.cropTop !== right.cropTop || left.cropBottom !== right.cropBottom) return "crop_mismatch";
  if (left.pixelExclusionThreshold !== right.pixelExclusionThreshold) return "threshold_mismatch";
  if (left.pNorm !== right.pNorm) return "pnorm_mismatch";
  return null;
}

function recipeForRun(run: Run, state: ProjectState): Recipe | null {
  const request = requestOf(run);
  const id = typeof request?.recipeId === "string" ? request.recipeId : null;
  return id ? state.recipesById[id] ?? null : null;
}

export type FusionEligibility = { ok: true } | { ok: false; reason: string };

export function fusionEligibility(
  run: Run,
  state: ProjectState,
  source: Source,
): FusionEligibility {
  if (run.runType !== "verification" && run.runType !== "verify") {
    return { ok: false, reason: "not_verification_run" };
  }
  if (run.status !== "completed") return { ok: false, reason: "status_not_completed" };
  if (run.sourceId !== source.id) return { ok: false, reason: "source_mismatch" };
  if (source.kind !== "video") return { ok: false, reason: "source_unavailable" };
  if (source.state === "missing" || source.state === "unsupported" || source.state === "error") {
    return { ok: false, reason: "source_unavailable" };
  }
  if (!source.fingerprint || !source.fingerprint.trim()) return { ok: false, reason: "source_fingerprint_missing" };
  const request = requestOf(run);
  if (!request) return { ok: false, reason: "input_snapshot_missing" };
  if (request.sourceId !== source.id) return { ok: false, reason: "source_mismatch" };
  if (request.sourceFingerprint !== source.fingerprint) return { ok: false, reason: "source_fingerprint_mismatch" };
  const streamIndex = request.scanScope && record(request.scanScope)?.streamIndex;
  if (!Number.isInteger(streamIndex) || (streamIndex as number) < 0) return { ok: false, reason: "stream_missing" };
  const recipe = recipeForRun(run, state);
  if (!recipe || !recipe.metric || !recipe.kernel || !recipe.geometry) return { ok: false, reason: "recipe_snapshot_missing" };
  if (!Number.isInteger(request.recipeRevision) || request.recipeRevision !== recipe.revision) return { ok: false, reason: "recipe_revision_mismatch" };
  if (!validMetric(request.metric)) return { ok: false, reason: "metric_mismatch" };
  const metricReason = metricDifference(request.metric, recipe.metric);
  if (metricReason) return { ok: false, reason: metricReason };
  if (request.axisMode !== recipe.axisMode) return { ok: false, reason: "axis_mode_mismatch" };
  if (request.profileId !== (recipe.profileId ?? "")) return { ok: false, reason: "profile_mismatch" };
  if (request.mathMode !== (recipe.mathMode ?? "raw")) return { ok: false, reason: "math_mode_mismatch" };
  const coverage = storedVerifyCoverage(run);
  const rawFrames = record(run.result)?.frames;
  if (Array.isArray(rawFrames)) {
    for (const rawFrame of rawFrames) {
      const row = record(rawFrame);
      const frameIndex = row?.frameIndex;
      const error = row?.error;
      if (!Number.isSafeInteger(frameIndex) || (frameIndex as number) < 0 || error === null || typeof error !== "number" || !Number.isFinite(error) || error < 0) {
        return { ok: false, reason: "invalid_frames" };
      }
    }
  }
  const frames = storedVerifyFrames(run);
  const expectedFrames = coverage?.processedFrames ?? run.completed;
  const selectedFrames = coverage?.selectedFrames ?? expectedFrames;
  const failedFrames = coverage?.failedFrames ?? 0;
  if (!frames || !frames.length || expectedFrames <= 0 || failedFrames !== 0 || expectedFrames !== selectedFrames || frames.length !== expectedFrames) {
    return { ok: false, reason: "metrics_incomplete" };
  }
  const seen = new Set<number>();
  for (const frame of frames) {
    if (!Number.isSafeInteger(frame.frameIndex) || frame.frameIndex < 0 || seen.has(frame.frameIndex)) {
      return { ok: false, reason: "invalid_frames" };
    }
    seen.add(frame.frameIndex);
    if (frame.error == null || !Number.isFinite(frame.error) || frame.error < 0) {
      return { ok: false, reason: "invalid_frames" };
    }
  }
  return { ok: true };
}

function runMetadata(run: Run, state: ProjectState): {
  recipeId: string;
  recipeRevision: number;
  recipe: Recipe;
  streamIndex: number;
  metric: MetricSpec;
  axisMode: string;
  profileId: string;
  mathMode: MathMode;
  scope: unknown;
} | null {
  const request = requestOf(run);
  const recipe = recipeForRun(run, state);
  const scope = record(request?.scanScope);
  if (!request || !recipe || !validMetric(request.metric) || !scope) return null;
  if (typeof request.recipeId !== "string" || !Number.isInteger(request.recipeRevision) || !Number.isInteger(scope.streamIndex)) return null;
  return {
    recipeId: request.recipeId,
    recipeRevision: request.recipeRevision as number,
    recipe,
    streamIndex: scope.streamIndex as number,
    metric: request.metric,
    axisMode: String(request.axisMode ?? ""),
    profileId: String(request.profileId ?? ""),
    mathMode: (request.mathMode === "log_display" ? "log_display" : "raw"),
    scope: request.scanScope,
  };
}

export type FusionBuildResult = { ok: true; fusion: VerificationFusion } | { ok: false; reason: string };

type PreparedFusion = {
  source: Source;
  orderedRuns: Run[];
  orderedMetas: Array<NonNullable<ReturnType<typeof runMetadata>>>;
};

/** Compatibility + ordering only. Does not allocate the per-frame union. */
export function prepareFusionRuns(input: {
  state: ProjectState;
  runIds: string[];
  sourceId: string;
}): { ok: true; prepared: PreparedFusion } | { ok: false; reason: string } {
  const source = input.state.sourcesById[input.sourceId];
  if (!source) return { ok: false, reason: "source_missing" };
  if (input.runIds.length < 2) return { ok: false, reason: "need_two_runs" };
  const runs = input.runIds.map((id) => input.state.runsById[id]);
  if (runs.some((run) => !run)) return { ok: false, reason: "run_missing" };
  const concreteRuns = runs as Run[];
  const metas = concreteRuns.map((run) => runMetadata(run, input.state));
  if (metas.some((meta) => !meta)) return { ok: false, reason: "input_snapshot_missing" };
  const concreteMetas = metas as Array<NonNullable<typeof metas[number]>>;
  const valid = concreteRuns.map((run) => fusionEligibility(run, input.state, source));
  const failed = valid.find((entry) => !entry.ok);
  if (failed && !failed.ok) return failed;
  const recipeIds = new Set(concreteMetas.map((meta) => meta.recipeId));
  if (recipeIds.size !== concreteMetas.length) return { ok: false, reason: "duplicate_recipe" };
  const order = concreteRuns
    .map((run, index) => ({ run, meta: concreteMetas[index] }))
    .sort((left, right) => recipeTie([left.meta, right.meta], left.meta.recipeId, right.meta.recipeId) || compareLex(left.run.id, right.run.id));
  const orderedRuns = order.map((item) => item.run);
  const orderedMetas = order.map((item) => item.meta);
  const first = orderedMetas[0];
  if (orderedMetas.some((meta) => meta.streamIndex !== first.streamIndex)) return { ok: false, reason: "stream_mismatch" };
  for (const meta of orderedMetas) {
    const metricReason = metricDifference(meta.metric, first.metric);
    if (metricReason) return { ok: false, reason: metricReason };
  }
  if (orderedMetas.some((meta) => meta.axisMode !== first.axisMode)) return { ok: false, reason: "axis_mode_mismatch" };
  if (orderedMetas.some((meta) => meta.profileId !== first.profileId)) return { ok: false, reason: "profile_mismatch" };
  if (orderedMetas.some((meta) => meta.mathMode !== first.mathMode)) return { ok: false, reason: "math_mode_mismatch" };
  return { ok: true, prepared: { source, orderedRuns, orderedMetas } };
}

export function buildVerificationFusion(input: {
  state: ProjectState;
  runIds: string[];
  sourceId: string;
  id?: string;
  createdAt?: string;
}): FusionBuildResult {
  const prepared = prepareFusionRuns(input);
  if (!prepared.ok) return prepared;
  const { source, orderedRuns, orderedMetas } = prepared.prepared;
  const first = orderedMetas[0];

  const byFrame = new Map<number, VerificationFusionCandidate[]>();
  orderedRuns.forEach((run, index) => {
    for (const frame of storedVerifyFrames(run) ?? []) {
      const candidates = byFrame.get(frame.frameIndex) ?? [];
      candidates.push({ runId: run.id, recipeId: orderedMetas[index].recipeId, error: frame.error as number });
      byFrame.set(frame.frameIndex, candidates);
    }
  });
  const frames: VerificationFusionFrame[] = [...byFrame.entries()].sort(([a], [b]) => a - b).map(([frameIndex, raw]) => {
    const candidates = [...raw].sort((a, b) => a.error - b.error || recipeTie(orderedMetas, a.recipeId, b.recipeId) || compareLex(a.runId, b.runId));
    const winner = candidates[0];
    return {
      frameIndex,
      fusedError: winner.error,
      winnerRunId: winner.runId,
      winnerRecipeId: winner.recipeId,
      candidateCount: candidates.length,
      candidates,
    };
  });
  const winsByRecipe: Record<string, number> = {};
  for (const meta of orderedMetas) winsByRecipe[meta.recipeId] = 0;
  let tiedFrames = 0;
  for (const frame of frames) {
    winsByRecipe[frame.winnerRecipeId] += 1;
    if (frame.candidates.filter((candidate) => candidate.error === frame.fusedError).length > 1) tiedFrames += 1;
  }
  const inputs: VerificationFusionInput[] = orderedMetas.map((meta, index) => ({
    runId: orderedRuns[index].id,
    recipeId: meta.recipeId,
    recipeRevision: meta.recipeRevision,
    recipeName: meta.recipe.name,
    recipeCreatedAt: meta.recipe.createdAt || null,
    recipeSnapshot: meta.recipe,
    scanScope: meta.scope,
  }));
  return {
    ok: true,
    fusion: {
      id: input.id ?? `fusion_${crypto.randomUUID()}`,
      createdAt: input.createdAt ?? new Date().toISOString(),
      sourceId: source.id,
      sourceFingerprint: source.fingerprint!,
      sourcePath: source.path,
      sourceLabel: source.label ?? null,
      streamIndex: first.streamIndex,
      algorithm: { name: "min_error_per_frame", version: 1, tieBreak: "recipe_created_at_asc" },
      compatibilitySnapshot: { metric: first.metric, axisMode: first.axisMode as AxisMode, profileId: first.profileId, mathMode: first.mathMode },
      inputs,
      frames,
      statistics: {
        totalFrames: frames.length,
        singleCandidateFrames: frames.filter((frame) => frame.candidateCount === 1).length,
        multiCandidateFrames: frames.filter((frame) => frame.candidateCount > 1).length,
        tiedFrames,
        winsByRecipe,
      },
    },
  };
}

function recipeTie(metas: Array<{ recipeId: string; recipe: Recipe }>, left: string, right: string): number {
  const l = metas.find((meta) => meta.recipeId === left)!;
  const r = metas.find((meta) => meta.recipeId === right)!;
  const lt = l.recipe.createdAt || "";
  const rt = r.recipe.createdAt || "";
  return (lt && rt ? compareLex(lt, rt) : 0) || compareLex(left, right);
}

function compareLex(left: string, right: string): number {
  return left < right ? -1 : left > right ? 1 : 0;
}

export function fusionSourceRuns(state: ProjectState, sourceId: string): Run[] {
  return Object.values(state.runsById)
    .filter((run) => run.sourceId === sourceId && (run.runType === "verification" || run.runType === "verify"))
    .sort((a, b) => b.createdAt.localeCompare(a.createdAt));
}
