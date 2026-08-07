import { invoke } from "@tauri-apps/api/core";
import { extractHeightSeries, metricCompatibilityKey } from "../engine/runGroupPlan";
import { extractKernelResultRows } from "../engine/kernelRunGroup";
import { extractVerifyFrames } from "../engine/verifyPlan";
import type { MetricSpec } from "../engine/protocol";
import type { ProjectState, Run } from "./types";

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

export { metricCompatibilityKey };
