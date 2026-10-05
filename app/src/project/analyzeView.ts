import type { MetricSpec } from "../engine/protocol";
import type { ProjectState } from "./types";

export type AnalyzeViewState = {
  metricSpecOpen: boolean;
  metric: MetricSpec | null;
  /** Last Resolution Test / Kernel Search parameter drafts, as stored JSON. */
  heightDraft: Record<string, unknown> | null;
  kernelDraft: Record<string, unknown> | null;
};

function readRecord(value: unknown): Record<string, unknown> | null {
  return value && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown> : null;
}

/**
 * Lay a stored draft over fresh defaults. Only keys the current draft shape
 * knows are taken, and only when the stored value has the same kind as the
 * default, so a draft saved by another version can never break the form.
 */
export function restoreDraft<T extends Record<string, unknown>>(
  defaults: T,
  stored: Record<string, unknown> | null,
  skip: ReadonlyArray<keyof T> = [],
): T {
  if (!stored) return defaults;
  const restored: Record<string, unknown> = { ...defaults };
  for (const key of Object.keys(defaults)) {
    if (skip.includes(key as keyof T) || !(key in stored)) continue;
    const fallback = defaults[key];
    const value = stored[key];
    const sameKind = Array.isArray(fallback)
      ? Array.isArray(value)
      : typeof value === typeof fallback && value !== null && !Array.isArray(value);
    if (sameKind) restored[key] = value;
  }
  return restored as T;
}

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
    return { metricSpecOpen: false, metric: null, heightDraft: null, kernelDraft: null };
  }
  const record = value as Record<string, unknown>;
  return {
    metricSpecOpen: record.metricSpecOpen === true,
    metric: parseStoredMetric(record.metric),
    heightDraft: readRecord(record.heightDraft),
    kernelDraft: readRecord(record.kernelDraft),
  };
}
