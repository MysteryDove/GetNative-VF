import { useCallback, useEffect, useMemo, useState } from "react";
import type { EngineEnvelope } from "../engine/types";
import {
  applyPreset,
  defaultHeightDraft,
  estimateHeightWork,
  fixedKernelsForDraft,
  resolveBackendPreference,
  resolveHeightGrid,
  selectableBackends,
  type HeightDraft,
} from "../engine/heightDraft";
import type { MetricSpec, SearchPreset } from "../engine/protocol";
import {
  planHeightRunGroup,
  type HeightRunGroupPlan,
} from "../engine/runGroupPlan";
import { pNormMaximumForBackend } from "../engine/backendSelection";
import type { ProjectState, Sample } from "../project/types";

/**
 * Height-scan draft state plus its pure derivations (grid, work estimate,
 * kernel/backend resolution, run-group plan). Seeding waits for engine
 * capabilities so defaults reflect the real backend.
 */
export function useHeightDraft({
  capabilities,
  includedSamples,
  sourcesById,
  subroute,
  initialMetric,
}: {
  capabilities: EngineEnvelope | null;
  includedSamples: Sample[];
  sourcesById: ProjectState["sourcesById"];
  /** Owned by the shell nav; only gates plan building (height subroute only). */
  subroute: "height" | "kernel";
  initialMetric?: MetricSpec | null;
}) {
  const [draft, setDraft] = useState<HeightDraft>(() => {
    const next = defaultHeightDraft(capabilities);
    if (initialMetric) next.metric = { ...initialMetric };
    return next;
  });
  const [draftSeeded, setDraftSeeded] = useState(Boolean(capabilities));

  useEffect(() => {
    if (draftSeeded || !capabilities) return;
    setDraft((current) => {
      const next = defaultHeightDraft(capabilities);
      next.metric = { ...current.metric };
      return next;
    });
    setDraftSeeded(true);
  }, [capabilities, draftSeeded]);

  const grid = useMemo(() => resolveHeightGrid(draft), [draft]);
  const work = useMemo(
    () => estimateHeightWork(draft, includedSamples.length, capabilities),
    [draft, includedSamples.length, capabilities],
  );
  const kernels = useMemo(
    () => fixedKernelsForDraft(draft, capabilities),
    [draft, capabilities],
  );
  const backends = useMemo(() => selectableBackends(capabilities), [capabilities]);
  const resolvedBackend = resolveBackendPreference(
    capabilities,
    draft.backendPreference,
    draft.metric.pNorm,
    draft.axisMode,
  );
  const pNormMaximum = pNormMaximumForBackend(capabilities, resolvedBackend);

  const planResult = useMemo(() => {
    if (subroute !== "height") return null;
    return planHeightRunGroup({
      draft,
      samples: includedSamples,
      sourcesById,
      capabilities,
    });
  }, [draft, includedSamples, sourcesById, capabilities, subroute]);

  const plan: HeightRunGroupPlan | null = planResult?.ok ? planResult.plan : null;

  const patch = useCallback((partial: Partial<HeightDraft>) => {
    setDraft((current) => ({ ...current, ...partial }));
  }, []);

  const setPreset = useCallback((preset: SearchPreset) => {
    setDraft((current) => applyPreset(current, preset));
  }, []);

  /** Refine the grid around a height picked from the results plot/table. */
  const refineAroundHeight = useCallback((height: string) => {
    setDraft((current) =>
      applyPreset(
        {
          ...current,
          refineSelected: height,
          refineHalfSpan: current.refineHalfSpan || "1.0",
          step: current.step.includes(".") ? current.step : "0.1",
        },
        "fractional_refine",
      ),
    );
  }, []);

  return {
    draft,
    patch,
    setPreset,
    refineAroundHeight,
    grid,
    work,
    kernels,
    backends,
    resolvedBackend,
    pNormMaximum,
    plan,
  };
}
