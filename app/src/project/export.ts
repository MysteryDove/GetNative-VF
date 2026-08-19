import { invoke } from "@tauri-apps/api/core";
import { extractHeightSeries, metricCompatibilityKey } from "../engine/runGroupPlan";
import { extractKernelResultRows } from "../engine/kernelRunGroup";
import { extractVerifyFrames } from "../engine/verifyPlan";
import type { MetricSpec } from "../engine/protocol";
import type { ProjectState, Run, VerificationFusion } from "./types";
import { snakeCaseSnapshot } from "./verificationFusion";

/**
 * Structured export of stored Run data. JSON carries full provenance; CSV
 * carries raw numeric series only. Both are built exclusively from stored
 * real records — no values are ever synthesized for export.
 */

export type ExportedRunRow = {
  runId: string;
  runType: string;
  seriesKey: string;
  metric: number;
};

export function runMetricSpec(run: Run): MetricSpec | null {
  const snapshot = run.inputSnapshot as { metric?: MetricSpec } | null;
  return snapshot?.metric ?? null;
}

/** Raw numeric rows for CSV export; empty when no real results exist. */
export function extractRunRows(run: Run): ExportedRunRow[] {
  if (run.runType === "height") {
    return (extractHeightSeries(run.result) ?? []).map((point) => ({
      runId: run.id,
      runType: run.runType,
      seriesKey: point.height,
      metric: point.metric,
    }));
  }
  if (run.runType === "kernel") {
    return (extractKernelResultRows(run.result) ?? []).map((entry) => ({
      runId: run.id,
      runType: run.runType,
      seriesKey: Object.keys(entry.parameters).length
        ? `${entry.kernelId}(${Object.entries(entry.parameters)
            .map(([key, value]) => `${key}=${value}`)
            .join(",")})`
        : entry.kernelId,
      metric: entry.metric,
    }));
  }
  if (run.runType === "verification" || run.runType === "verify") {
    return (extractVerifyFrames(run.result) ?? []).map((frame) => ({
      runId: run.id,
      runType: run.runType,
      seriesKey: String(frame.frameIndex),
      metric: frame.metric,
    }));
  }
  return [];
}

export function buildRunExportJson(state: ProjectState, runIds: string[]): string {
  const runs = runIds
    .map((id) => state.runsById[id])
    .filter((run): run is Run => Boolean(run));
  const groupIds = [...new Set(runs.map((run) => run.runGroupId).filter(Boolean))] as string[];
  const payload = {
    schema_version: 1,
    kind: "getnative_run_export",
    project: {
      id: state.project.id,
      name: state.project.name,
    },
    run_groups: groupIds.map((id) => state.runGroupsById[id]).filter(Boolean),
    runs: runs.map((run) => ({
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
  };
  return JSON.stringify(payload, null, 2);
}

export function buildRunExportCsv(runIds: string[], state: ProjectState): string | null {
  const header = "run_id,run_type,series_key,metric";
  const rows: string[] = [];
  for (const id of runIds) {
    const run = state.runsById[id];
    if (!run) continue;
    for (const row of extractRunRows(run)) {
      const key = row.seriesKey.includes(",") ? `"${row.seriesKey.replace(/"/g, '""')}"` : row.seriesKey;
      rows.push(`${row.runId},${row.runType},${key},${row.metric}`);
    }
  }
  if (!rows.length) return null;
  return [header, ...rows].join("\n") + "\n";
}

/** True when at least one selected Run has exportable raw rows. */
export function hasExportableRows(state: ProjectState, runIds: string[]): boolean {
  return runIds.some((id) => {
    const run = state.runsById[id];
    return run ? extractRunRows(run).length > 0 : false;
  });
}

export async function saveArtifact(input: {
  defaultName: string;
  extension: "json" | "csv";
  content: string;
}): Promise<string> {
  return invoke<string>("export_artifact", {
    request: {
      defaultName: input.defaultName,
      extension: input.extension,
      content: input.content,
    },
  });
}

export function buildVerificationFusionJson(fusion: VerificationFusion): string {
  const payload = {
    export_schema_version: 1,
    kind: "getnative_verification_fusion",
    fusion: {
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
      inputs: fusion.inputs.map((input) => ({
        run_id: input.runId,
        recipe_id: input.recipeId,
        recipe_revision: input.recipeRevision,
        recipe_name: input.recipeName,
        recipe_created_at: input.recipeCreatedAt,
        recipe_snapshot: snakeCaseSnapshot(input.recipeSnapshot),
        scan_scope: snakeCaseSnapshot(input.scanScope),
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
    },
  };
  return JSON.stringify(payload, null, 2);
}

function csvCell(value: string | number | boolean | null): string {
  const text = value == null ? "" : String(value);
  return /[",\r\n]/.test(text) ? `"${text.replace(/"/g, '""')}"` : text;
}

export function buildVerificationFusionCsv(fusion: VerificationFusion): string {
  const header = [
    "fusion_id", "source_id", "frame_index", "fused_error", "winner_run_id", "winner_recipe_id",
    "winner_recipe_name", "candidate_count", "candidate_run_id", "candidate_recipe_id",
    "candidate_recipe_name", "candidate_error", "is_winner",
  ];
  const recipeNames = new Map(fusion.inputs.map((input) => [input.recipeId, input.recipeName]));
  const rows = fusion.frames.flatMap((frame) => frame.candidates.map((candidate) => [
    fusion.id,
    fusion.sourceId,
    frame.frameIndex,
    frame.fusedError,
    frame.winnerRunId,
    frame.winnerRecipeId,
    recipeNames.get(frame.winnerRecipeId) ?? "",
    frame.candidateCount,
    candidate.runId,
    candidate.recipeId,
    recipeNames.get(candidate.recipeId) ?? "",
    candidate.error,
    candidate.runId === frame.winnerRunId && candidate.recipeId === frame.winnerRecipeId && candidate.error === frame.fusedError,
  ].map((value) => csvCell(value)).join(",")));
  return [header.join(","), ...rows].join("\n") + "\n";
}

export { metricCompatibilityKey };
