import type { MetricSpec } from "../engine/protocol";
import type { ProjectState } from "./types";

export type AnalyzeViewState = {
  metricSpecOpen: boolean;
  metric: MetricSpec | null;
};

function readNumber(value: unknown): number | null {
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

export function parseStoredMetric(value: unknown): MetricSpec | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const record = value as Record<string, unknown>;
  const cropLeft = readNumber(record.cropLeft);
  const cropRight = readNumber(record.cropRight);
  const cropTop = readNumber(record.cropTop);
  const cropBottom = readNumber(record.cropBottom);
  const pixelExclusionThreshold = readNumber(record.pixelExclusionThreshold);
  const pNorm = readNumber(record.pNorm);
  if (
    cropLeft == null
    || cropRight == null
    || cropTop == null
    || cropBottom == null
    || pixelExclusionThreshold == null
    || pNorm == null
  ) {
    return null;
  }
  return { cropLeft, cropRight, cropTop, cropBottom, pixelExclusionThreshold, pNorm };
}

export function analyzeViewState(state: ProjectState): AnalyzeViewState {
  const value = state.uiStateByRoute.analyze;
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    return { metricSpecOpen: false, metric: null };
  }
  const record = value as { metricSpecOpen?: unknown; metric?: unknown };
  return {
    metricSpecOpen: record.metricSpecOpen === true,
    metric: parseStoredMetric(record.metric),
  };
}
