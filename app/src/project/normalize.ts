import type {
  OpenedProjectDto,
  ProjectManifestDto,
  ProjectRoute,
  ProjectState,
  Recipe,
  Run,
  RunGroup,
  Sample,
  Source,
  VerificationReview,
  VerificationFusion,
} from "./types";
import { profileFor } from "../engine/profiles";
import { recipeReadiness } from "./recipe";
import { migrateGeometrySnapshot } from "../engine/geometry";
import { snakeCaseSnapshot } from "./verificationFusion";

const projectRoutes: ProjectRoute[] = [
  "overview",
  "media",
  "samples",
  "analyze",
  "verify",
  "results",
  "settings",
  "diagnostics",
];

function indexById<T extends { id: string }>(items: T[]): Record<string, T> {
  return items.reduce<Record<string, T>>((acc, item) => {
    acc[item.id] = item;
    return acc;
  }, {});
}

export function emptyProjectState(partial?: Partial<ProjectState["project"]>): ProjectState {
  return {
    project: {
      id: partial?.id ?? "",
      name: partial?.name ?? "",
      schemaVersion: partial?.schemaVersion ?? 2,
      createdAt: partial?.createdAt ?? "",
      updatedAt: partial?.updatedAt ?? "",
      activeRecipeId: partial?.activeRecipeId ?? null,
      untitled: partial?.untitled ?? false,
      storagePath: partial?.storagePath ?? null,
      readOnly: partial?.readOnly ?? false,
      dirty: partial?.dirty ?? false,
    },
    sourcesById: {},
    samplesById: {},
    recipesById: {},
    runGroupsById: {},
    runsById: {},
    verificationReviewsByRunId: {},
    verificationFusionsById: {},
    uiStateByRoute: {},
  };
}

export function openedToProjectState(opened: OpenedProjectDto): ProjectState {
  const { manifest } = opened;
  const sources: Source[] = manifest.sources.map((source) => ({
    id: source.id,
    kind: source.kind,
    path: source.path,
    fingerprint: source.fingerprint ?? null,
    state: source.state ?? "added",
    label: source.label ?? null,
    sizeBytes: source.size_bytes ?? null,
    width: source.width ?? null,
    height: source.height ?? null,
    durationSeconds: source.duration_seconds ?? null,
    decoder: source.decoder ?? null,
    videoStreams: (source.video_streams ?? []).map((stream) => ({
      index: stream.index,
      codecName: stream.codec_name ?? null,
      width: stream.width ?? null,
      height: stream.height ?? null,
      durationSeconds: stream.duration_seconds ?? null,
      frameCount: stream.frame_count ?? null,
      timeBaseNum: stream.time_base_num ?? null,
      timeBaseDen: stream.time_base_den ?? null,
      frameRateNum: stream.frame_rate_num ?? null,
      frameRateDen: stream.frame_rate_den ?? null,
    })),
    selectedStreamIndex: source.selected_stream_index ?? null,
    errorCode: source.error_code ?? null,
    errorDetail: source.error_detail ?? null,
  }));

  // Missing paths are already marked by Rust; reinforce for UI readiness.
  for (const missingId of opened.missing_source_ids) {
    const source = sources.find((item) => item.id === missingId);
    if (source) {
      source.state = "missing";
    }
  }

  const samples: Sample[] = manifest.samples.map((sample) => ({
    id: sample.id,
    sourceId: sample.source_id,
    sourceFingerprint: sample.source_fingerprint ?? null,
    label: sample.label ?? null,
    included: sample.included ?? true,
    order: sample.order ?? 0,
    frameIndex: sample.frame_index ?? null,
    streamIndex: sample.stream_index ?? null,
    pts: sample.pts ?? null,
    bestEffortTimestamp: sample.best_effort_timestamp ?? null,
    timeBaseNum: sample.time_base_num ?? null,
    timeBaseDen: sample.time_base_den ?? null,
    timestampSeconds: sample.timestamp_seconds ?? null,
    tags: sample.tags ?? [],
    originRunId: sample.origin_run_id ?? null,
  }));

  const recipes: Recipe[] = manifest.recipes.map((recipe) => {
    // Schema-1 compatibility: `locked` predates `status`; a missing status
    // derives from it, and an explicit status always wins. Both locked and
    // superseded Recipes are immutable and serialize locked=true.
    const status =
      recipe.status ?? (recipe.locked ? "locked" : "draft");
    return {
      id: recipe.id,
      name: recipe.name,
      nameSuffix: recipe.name_suffix ?? null,
      status,
      locked: status !== "draft",
      revision: recipe.revision ?? 1,
      parentRecipeId: recipe.parent_recipe_id ?? null,
      createdAt: recipe.created_at ?? "",
      updatedAt: recipe.updated_at ?? "",
      geometry: migrateGeometrySnapshot(recipe.geometry),
      kernel: recipe.kernel ?? null,
      metric: recipe.metric ?? null,
      axisMode: recipe.axis_mode ?? profileFor(recipe.profile_id ?? "").default_axis_mode,
      profileId: recipe.profile_id ?? null,
      mathMode: recipe.math_mode ?? null,
    };
  });

  const runGroups: RunGroup[] = manifest.run_groups.map((group) => ({
    id: group.id,
    memberRunIds: group.member_run_ids ?? [],
    groupType: group.group_type ?? "",
    label: group.label ?? "",
    createdAt: group.created_at ?? "",
    intentSnapshot: group.intent_snapshot ?? null,
  }));

  const runs: Run[] = manifest.runs.map((run) => ({
    id: run.id,
    runType: run.run_type ?? "",
    status: run.status ?? "",
    runGroupId: run.run_group_id ?? null,
    sampleId: run.sample_id ?? null,
    sourceId: run.source_id ?? null,
    createdAt: run.created_at ?? "",
    updatedAt: run.updated_at ?? "",
    inputSnapshot: run.input_snapshot ?? null,
    result: run.result ?? null,
    errorCode: run.error_code ?? null,
    errorMessage: run.error_message ?? null,
    completed: run.completed ?? 0,
    total: run.total ?? 0,
  }));

  const reviews: VerificationReview[] = manifest.verification_reviews.map((review) => ({
    runId: review.run_id,
    tags: review.tags ?? [],
  }));

  const fusions: VerificationFusion[] = (manifest.verification_fusions ?? []).map((fusion) => ({
    id: fusion.id,
    createdAt: fusion.created_at,
    sourceId: fusion.source_id,
    sourceFingerprint: fusion.source_fingerprint,
    sourcePath: fusion.source_path,
    sourceLabel: fusion.source_label ?? null,
    streamIndex: fusion.stream_index,
    algorithm: {
      name: fusion.algorithm.name,
      version: fusion.algorithm.version,
      tieBreak: fusion.algorithm.tie_break,
    },
    compatibilitySnapshot: {
      metric: {
        cropLeft: fusion.compatibility_snapshot.metric.crop_left,
        cropRight: fusion.compatibility_snapshot.metric.crop_right,
        cropTop: fusion.compatibility_snapshot.metric.crop_top,
        cropBottom: fusion.compatibility_snapshot.metric.crop_bottom,
        pixelExclusionThreshold: fusion.compatibility_snapshot.metric.pixel_exclusion_threshold,
        pNorm: fusion.compatibility_snapshot.metric.p_norm,
      },
      axisMode: fusion.compatibility_snapshot.axis_mode,
      profileId: fusion.compatibility_snapshot.profile_id,
      mathMode: fusion.compatibility_snapshot.math_mode,
    },
    inputs: fusion.inputs.map((item) => ({
      runId: item.run_id,
      recipeId: item.recipe_id,
      recipeRevision: item.recipe_revision,
      recipeName: item.recipe_name,
      recipeCreatedAt: item.recipe_created_at,
      recipeSnapshot: item.recipe_snapshot,
      scanScope: item.scan_scope,
    })),
    frames: fusion.frames.map((frame) => ({
      frameIndex: frame.frame_index,
      fusedError: frame.fused_error,
      winnerRunId: frame.winner_run_id,
      winnerRecipeId: frame.winner_recipe_id,
      candidateCount: frame.candidate_count,
      candidates: frame.candidates.map((candidate) => ({
        runId: candidate.run_id,
        recipeId: candidate.recipe_id,
        error: candidate.error,
      })),
    })),
    statistics: {
      totalFrames: fusion.statistics.total_frames,
      singleCandidateFrames: fusion.statistics.single_candidate_frames,
      multiCandidateFrames: fusion.statistics.multi_candidate_frames,
      tiedFrames: fusion.statistics.tied_frames,
      winsByRecipe: fusion.statistics.wins_by_recipe,
    },
  }));

  const recipesById = indexById(recipes);

  return {
    project: {
      id: manifest.id,
      name: manifest.name,
      schemaVersion: manifest.schema_version,
      createdAt: manifest.created_at,
      updatedAt: manifest.updated_at,
      activeRecipeId: manifest.active_recipe_id,
      untitled: manifest.untitled,
      storagePath: opened.storage_path,
      readOnly: opened.read_only,
      dirty: false,
    },
    sourcesById: indexById(sources),
    samplesById: indexById(samples),
    recipesById,
    runGroupsById: indexById(runGroups),
    runsById: indexById(runs),
    verificationReviewsByRunId: reviews.reduce<Record<string, VerificationReview>>((acc, review) => {
      acc[review.runId] = review;
      return acc;
    }, {}),
    verificationFusionsById: indexById(fusions),
    uiStateByRoute: manifest.ui_state_by_route ?? {},
  };
}

export function projectStateToManifest(state: ProjectState): ProjectManifestDto {
  return {
    schema_version: state.project.schemaVersion,
    id: state.project.id,
    name: state.project.name,
    created_at: state.project.createdAt,
    updated_at: state.project.updatedAt,
    active_recipe_id: state.project.activeRecipeId,
    untitled: state.project.untitled,
    sources: Object.values(state.sourcesById).map((source) => ({
      id: source.id,
      kind: source.kind,
      path: source.path,
      fingerprint: source.fingerprint ?? null,
      state: source.state,
      label: source.label ?? null,
      size_bytes: source.sizeBytes ?? null,
      width: source.width ?? null,
      height: source.height ?? null,
      duration_seconds: source.durationSeconds ?? null,
      decoder: source.decoder ?? null,
      video_streams: source.videoStreams.map((stream) => ({
        index: stream.index,
        codec_name: stream.codecName ?? null,
        width: stream.width ?? null,
        height: stream.height ?? null,
        duration_seconds: stream.durationSeconds ?? null,
        frame_count: stream.frameCount ?? null,
        time_base_num: stream.timeBaseNum ?? null,
        time_base_den: stream.timeBaseDen ?? null,
        frame_rate_num: stream.frameRateNum ?? null,
        frame_rate_den: stream.frameRateDen ?? null,
      })),
      selected_stream_index: source.selectedStreamIndex ?? null,
      error_code: source.errorCode ?? null,
      error_detail: source.errorDetail ?? null,
    })),
    samples: Object.values(state.samplesById)
      .sort((a, b) => a.order - b.order)
      .map((sample) => ({
        id: sample.id,
        source_id: sample.sourceId,
        source_fingerprint: sample.sourceFingerprint ?? null,
        label: sample.label ?? null,
        included: sample.included,
        order: sample.order,
        frame_index: sample.frameIndex ?? null,
        stream_index: sample.streamIndex ?? null,
        pts: sample.pts ?? null,
        best_effort_timestamp: sample.bestEffortTimestamp ?? null,
        time_base_num: sample.timeBaseNum ?? null,
        time_base_den: sample.timeBaseDen ?? null,
        timestamp_seconds: sample.timestampSeconds ?? null,
        tags: sample.tags,
        origin_run_id: sample.originRunId ?? null,
      })),
    recipes: Object.values(state.recipesById).map((recipe) => ({
      id: recipe.id,
      name: recipe.name,
      name_suffix: recipe.nameSuffix ?? null,
      locked: recipe.status !== "draft",
      status: recipe.status,
      revision: recipe.revision,
      parent_recipe_id: recipe.parentRecipeId,
      created_at: recipe.createdAt,
      updated_at: recipe.updatedAt,
      geometry: recipe.geometry,
      kernel: recipe.kernel,
      metric: recipe.metric,
      axis_mode: recipe.axisMode,
      profile_id: recipe.profileId,
      math_mode: recipe.mathMode,
    })),
    run_groups: Object.values(state.runGroupsById).map((group) => ({
      id: group.id,
      member_run_ids: group.memberRunIds,
      group_type: group.groupType,
      label: group.label,
      created_at: group.createdAt,
      intent_snapshot: group.intentSnapshot,
    })),
    runs: Object.values(state.runsById).map((run) => ({
      id: run.id,
      run_type: run.runType,
      status: run.status,
      run_group_id: run.runGroupId,
      sample_id: run.sampleId,
      source_id: run.sourceId,
      created_at: run.createdAt,
      updated_at: run.updatedAt,
      input_snapshot: run.inputSnapshot,
      result: run.result,
      error_code: run.errorCode,
      error_message: run.errorMessage,
      completed: run.completed,
      total: run.total,
    })),
    verification_reviews: Object.values(state.verificationReviewsByRunId).map((review) => ({
      run_id: review.runId,
      tags: review.tags,
    })),
    verification_fusions: Object.values(state.verificationFusionsById).map((fusion) => ({
      id: fusion.id,
      created_at: fusion.createdAt,
      source_id: fusion.sourceId,
      source_fingerprint: fusion.sourceFingerprint,
      source_path: fusion.sourcePath,
      source_label: fusion.sourceLabel,
      stream_index: fusion.streamIndex,
      algorithm: {
        name: fusion.algorithm.name,
        version: fusion.algorithm.version,
        tie_break: fusion.algorithm.tieBreak,
      },
      compatibility_snapshot: {
        metric: {
          crop_left: fusion.compatibilitySnapshot.metric.cropLeft,
          crop_right: fusion.compatibilitySnapshot.metric.cropRight,
          crop_top: fusion.compatibilitySnapshot.metric.cropTop,
          crop_bottom: fusion.compatibilitySnapshot.metric.cropBottom,
          pixel_exclusion_threshold: fusion.compatibilitySnapshot.metric.pixelExclusionThreshold,
          p_norm: fusion.compatibilitySnapshot.metric.pNorm,
        },
        axis_mode: fusion.compatibilitySnapshot.axisMode,
        profile_id: fusion.compatibilitySnapshot.profileId,
        math_mode: fusion.compatibilitySnapshot.mathMode,
      },
      inputs: fusion.inputs.map((item) => ({
        run_id: item.runId,
        recipe_id: item.recipeId,
        recipe_revision: item.recipeRevision,
        recipe_name: item.recipeName,
        recipe_created_at: item.recipeCreatedAt,
        recipe_snapshot: snakeCaseSnapshot(item.recipeSnapshot),
        scan_scope: snakeCaseSnapshot(item.scanScope),
      })),
      frames: fusion.frames.map((frame) => ({
        frame_index: frame.frameIndex,
        fused_error: frame.fusedError,
        winner_run_id: frame.winnerRunId,
        winner_recipe_id: frame.winnerRecipeId,
        candidate_count: frame.candidateCount,
        candidates: frame.candidates.map((candidate) => ({
          run_id: candidate.runId,
          recipe_id: candidate.recipeId,
          error: candidate.error,
        })),
      })),
      statistics: {
        total_frames: fusion.statistics.totalFrames,
        single_candidate_frames: fusion.statistics.singleCandidateFrames,
        multi_candidate_frames: fusion.statistics.multiCandidateFrames,
        tied_frames: fusion.statistics.tiedFrames,
        wins_by_recipe: fusion.statistics.winsByRecipe,
      },
    })),
    ui_state_by_route: state.uiStateByRoute,
  };
}

export function countById(map: Record<string, unknown>): number {
  return Object.keys(map).length;
}

export function missingSourceCount(state: ProjectState): number {
  return Object.values(state.sourcesById).filter((source) => source.state === "missing").length;
}

export function restoredProjectRoute(
  state: ProjectState,
  fallback: ProjectRoute,
): ProjectRoute {
  const shell = state.uiStateByRoute.shell;
  if (!shell || typeof shell !== "object" || Array.isArray(shell)) return fallback;
  const lastRoute = (shell as { lastRoute?: unknown }).lastRoute;
  return typeof lastRoute === "string" && projectRoutes.includes(lastRoute as ProjectRoute)
    ? (lastRoute as ProjectRoute)
    : fallback;
}

export function readiness(state: ProjectState, analyzeAvailable: boolean) {
  const sourceCount = countById(state.sourcesById);
  const includedSamples = Object.values(state.samplesById).filter((sample) => sample.included);
  const sampleCount = includedSamples.length;
  const missing = missingSourceCount(state);
  const hasUnavailableIncludedSamples = includedSamples.some(
    (sample) => {
      const source = state.sourcesById[sample.sourceId];
      return (
        source?.state !== "ready" ||
        Boolean(
          sample.sourceFingerprint &&
            source.fingerprint &&
            sample.sourceFingerprint !== source.fingerprint,
        )
      );
    },
  );
  const readyVideoCount = Object.values(state.sourcesById).filter(
    (source) => source.kind === "video" && source.state === "ready",
  ).length;
  const current = state.project.activeRecipeId
    ? state.recipesById[state.project.activeRecipeId]
    : null;
  const hasCurrentRecipe = Boolean(current);
  const currentRecipeReady = Boolean(current && recipeReadiness(current).ok);

  return {
    hasMedia: sourceCount > 0,
    hasMissingMedia: missing > 0,
    hasSamples: sampleCount > 0,
    hasUnavailableIncludedSamples,
    hasReadyVideo: readyVideoCount > 0,
    heightReady: analyzeAvailable && sampleCount > 0 && !hasUnavailableIncludedSamples,
    kernelReady: analyzeAvailable && sampleCount > 0 && !hasUnavailableIncludedSamples,
    hasActiveRecipe: hasCurrentRecipe,
    verifyReady: analyzeAvailable && currentRecipeReady && readyVideoCount > 0,
  };
}
