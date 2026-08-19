import { useEffect, useMemo } from "react";
import type { EngineEnvelope } from "../engine/types";
import type { MetricSpec } from "../engine/protocol";
import {
  estimateKernelWork,
  geometryGroupKey,
  resolveKernelCandidates,
  type KernelDraft,
  type ResolvedGeometryMap,
} from "../engine/kernelDraft";
import {
  buildKernelResultRows,
  compareKernelResultRows,
  planKernelRunGroup,
} from "../engine/kernelRunGroup";
import { metricCompatibilityKey } from "../engine/runGroupPlan";
import { profileFor } from "../engine/profiles";
import { resolveBackendPreference, validateBackendPNorm } from "../engine/heightDraft";
import { geometryForSource } from "../engine/geometry";
import { pNormMaximumForBackend } from "../engine/backendSelection";
import { activeRecipe } from "../project/recipe";
import {
  includedSamples as selectIncludedSamples,
  resultSampleIsVisible,
} from "../project/samples";
import type { ProjectState } from "../project/types";

export type SampleDims = { width: number; height: number };

/**
 * Derives everything the Kernel Analyze panel renders from the draft plus
 * project state, and keeps the draft mirrored to its inherited inputs:
 * the Recipe geometry/profile mirror and the Height-metric inheritance are
 * value-equality guarded so they never cause a render loop.
 */
export function useKernelPlan({
  state,
  capabilities,
  draft,
  onDraftChange,
  inheritMetric,
  inheritedMetric,
  excludedSampleIds,
  selectedResultKey,
  sampleFilter,
  showExcludedResults,
  onSelectResultKey,
}: {
  state: ProjectState;
  capabilities: EngineEnvelope | null;
  draft: KernelDraft;
  onDraftChange: (updater: (current: KernelDraft) => KernelDraft) => void;
  inheritMetric: boolean;
  inheritedMetric: MetricSpec;
  /** Samples excluded from the kernel test (default: every included sample). */
  excludedSampleIds: Set<string>;
  /** Selected result row (`${runId}-${kernelLabel}`), mirrored for row highlighting. */
  selectedResultKey: string | null;
  /** Result table sample switch: null = all samples. */
  sampleFilter: string | null;
  showExcludedResults: boolean;
  onSelectResultKey: (updater: (current: string | null) => string | null) => void;
}) {
  // MetricSpec inherits from Height by default; an explicit unlink is visible.
  // Value-equality guard (mirrors the geometry-mirror effect below) prevents a
  // render loop: skip the update when the draft already matches.
  useEffect(() => {
    if (!inheritMetric) return;
    const metric = draft.metric;
    if (
      metric.cropLeft === inheritedMetric.cropLeft &&
      metric.cropRight === inheritedMetric.cropRight &&
      metric.cropTop === inheritedMetric.cropTop &&
      metric.cropBottom === inheritedMetric.cropBottom &&
      metric.pixelExclusionThreshold === inheritedMetric.pixelExclusionThreshold &&
      metric.pNorm === inheritedMetric.pNorm
    ) {
      return;
    }
    onDraftChange((current) => ({ ...current, metric: { ...inheritedMetric } }));
  }, [inheritMetric, inheritedMetric, draft.metric, onDraftChange]);

  const includedSamples = useMemo(
    () => selectIncludedSamples(state),
    [state.samplesById],
  );

  const sampleDims = useMemo(() => {
    const map: Record<string, SampleDims | null> = {};
    for (const sample of includedSamples) {
      const source = state.sourcesById[sample.sourceId];
      map[sample.id] =
        source?.width && source?.height
          ? { width: source.width, height: source.height }
          : null;
    }
    return map;
  }, [includedSamples, state.sourcesById]);

  /** The samples the kernel test actually runs on. */
  const testSamples = useMemo(
    () => includedSamples.filter((sample) => !excludedSampleIds.has(sample.id)),
    [includedSamples, excludedSampleIds],
  );

  /** The kernel test is pinned to the current Recipe's geometry. */
  const currentRecipe = activeRecipe(state);
  const recipeGeometry = currentRecipe?.geometry ?? null;
  const recipeProfileId = currentRecipe?.profileId ?? null;
  const kernelAxisMode = currentRecipe?.axisMode ??
    profileFor(draft.profileId, capabilities).default_axis_mode;

  // Mirror the Recipe geometry's base size (and profile) into the draft so
  // per-source-shape group keys line up with the plan's key derivation.
  useEffect(() => {
    if (!recipeGeometry) return;
    const baseHeight = recipeGeometry.baseHeight != null ? String(recipeGeometry.baseHeight) : "";
    const baseWidth =
      recipeGeometry.baseWidth != null ? String(recipeGeometry.baseWidth) : "";
    if (
      draft.baseHeight === baseHeight &&
      draft.baseWidth === baseWidth &&
      (!recipeProfileId || draft.profileId === recipeProfileId)
    ) {
      return;
    }
    onDraftChange((current) => ({
      ...current,
      baseHeight,
      baseWidth,
      ...(recipeProfileId ? { profileId: recipeProfileId } : {}),
    }));
  }, [recipeGeometry, recipeProfileId, draft.baseHeight, draft.baseWidth, draft.profileId, onDraftChange]);

  /** Distinct source shapes whose fixed geometry must be resolved. */
  const geometryGroups = useMemo(() => {
    const groups = new Map<string, { key: string; dims: SampleDims }>();
    for (const sample of includedSamples) {
      const dims = sampleDims[sample.id];
      if (!dims) continue;
      const key = geometryGroupKey({
        sourceWidth: dims.width,
        sourceHeight: dims.height,
        baseHeight: draft.baseHeight,
        baseWidth: draft.baseWidth,
        profileId: draft.profileId,
      });
      if (!groups.has(key)) groups.set(key, { key, dims });
    }
    return [...groups.values()];
  }, [includedSamples, sampleDims, draft.baseHeight, draft.baseWidth, draft.profileId]);

  /** Resolve the locked src/base semantics independently for every source shape. */
  const geometries = useMemo<ResolvedGeometryMap>(() => {
    if (!recipeGeometry) return {};
    return Object.fromEntries(
      geometryGroups.map((group) => [
        group.key,
        geometryForSource(recipeGeometry, currentRecipe?.axisMode ?? "h_plus_w", group.dims.width, group.dims.height),
      ]),
    );
  }, [geometryGroups, recipeGeometry, currentRecipe?.axisMode]);

  const candidates = useMemo(
    () => resolveKernelCandidates(draft, capabilities),
    [draft, capabilities],
  );

  const plan = useMemo(() => {
    const result = planKernelRunGroup({
      draft,
      samples: testSamples,
      sourcesById: state.sourcesById,
      geometries,
      capabilities,
      axisMode: kernelAxisMode,
    });
    return result.ok ? result.plan : null;
  }, [draft, testSamples, state.sourcesById, geometries, capabilities, kernelAxisMode]);

  const resolvedBackend = resolveBackendPreference(
    capabilities,
    draft.backendPreference,
    draft.metric.pNorm,
    kernelAxisMode,
  );
  const pNormMaximum = pNormMaximumForBackend(capabilities, resolvedBackend);
  const pNormSupported = validateBackendPNorm(
    capabilities,
    draft.backendPreference,
    draft.metric.pNorm,
    kernelAxisMode,
  ).ok;

  const work = candidates.ok
    ? estimateKernelWork({
        sampleCount: testSamples.length,
        candidateCount: candidates.candidates.length,
      })
    : 0;

  const kernelRuns = useMemo(
    () =>
      Object.values(state.runsById)
        .filter(
          (run) =>
            run.runType === "kernel" &&
            resultSampleIsVisible(state.samplesById, run.sampleId, showExcludedResults),
        )
        .sort((a, b) => b.createdAt.localeCompare(a.createdAt)),
    [showExcludedResults, state.runsById, state.samplesById],
  );
  const activeMetricKey = metricCompatibilityKey(draft.metric);
  const resultRows = useMemo(
    () => buildKernelResultRows(kernelRuns, state, activeMetricKey),
    [kernelRuns, state, activeMetricKey],
  );
  const kernelTableRows = useMemo(
    () =>
      [...resultRows.rows].sort(compareKernelResultRows).map((row) => {
        const key = `${row.runId}-${row.kernelLabel}`;
        return {
          key,
          metric: row.metric,
          sampleId: row.sampleId,
          cells: [row.kernelLabel, row.sampleLabel, row.runId.slice(0, 10)],
          selected: selectedResultKey === key,
          onSelect: () =>
            onSelectResultKey((current) => (current === key ? null : key)),
        };
      }),
    [resultRows, selectedResultKey, onSelectResultKey],
  );
  /** Distinct samples present in the result rows, for the table switch. */
  const resultSamples = useMemo(() => {
    const seen = new Map<string, string>();
    for (const row of resultRows.rows) {
      if (!seen.has(row.sampleId)) seen.set(row.sampleId, row.sampleLabel);
    }
    return [...seen.entries()].map(([id, label]) => ({ id, label }));
  }, [resultRows]);
  const visibleTableRows = useMemo(
    () =>
      sampleFilter
        ? kernelTableRows.filter((row) => row.sampleId === sampleFilter)
        : kernelTableRows,
    [kernelTableRows, sampleFilter],
  );

  return {
    currentRecipe,
    recipeGeometry,
    includedSamples,
    sampleDims,
    testSamples,
    candidates,
    plan,
    kernelAxisMode,
    resolvedBackend,
    pNormMaximum,
    pNormSupported,
    work,
    resultRows,
    resultSamples,
    visibleTableRows,
  };
}
