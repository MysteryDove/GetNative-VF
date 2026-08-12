import { useCallback, useEffect, useMemo, useState } from "react";
import type { EngineEnvelope } from "../engine/types";
import {
  applyPreset,
  applyProfileDefaults,
  defaultHeightDraft,
  estimateHeightWork,
  fixedKernelsForDraft,
  resolveBackendPreference,
  resolveHeightGrid,
  selectableBackends,
  type HeightDraft,
} from "../engine/heightDraft";
import type { SearchPreset } from "../engine/protocol";
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
}: {
  capabilities: EngineEnvelope | null;
  includedSamples: Sample[];
  sourcesById: ProjectState["sourcesById"];
}) {
  const [draft, setDraft] = useState<HeightDraft>(() => defaultHeightDraft(capabilities));
  const [draftSeeded, setDraftSeeded] = useState(Boolean(capabilities));

  useEffect(() => {
    if (draftSeeded || !capabilities) return;
    setDraft(defaultHeightDraft(capabilities));
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
    if (draft.subroute !== "height") return null;
    return planHeightRunGroup({
      draft,
      samples: includedSamples,
      sourcesById,
      capabilities,
    });
  }, [draft, includedSamples, sourcesById, capabilities]);

  const plan: HeightRunGroupPlan | null = planResult?.ok ? planResult.plan : null;

  const patch = useCallback((partial: Partial<HeightDraft>) => {
    setDraft((current) => ({ ...current, ...partial }));
  }, []);

  const setPreset = useCallback((preset: SearchPreset) => {
    setDraft((current) => applyPreset(current, preset));
  }, []);

  const resetToProfileDefaults = useCallback(() => {
    setDraft((current) => applyProfileDefaults(current, capabilities));
  }, [capabilities]);

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
    resetToProfileDefaults,
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
