import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { EngineEnvelope } from "../engine/types";
import {
  applyPreset,
  convertScanRange,
  scanAxisKind,
  type ScanAxisKind,
  type ScanRange,
  defaultHeightDraft,
  estimateHeightWork,
  fixedKernelsForDraft,
  resolveBackendPreference,
  resolveHeightGrid,
  scansFractionalCandidates,
  selectableBackends,
  withFractionalBase,
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

  // Start/stop are px of the scanned axis, so each axis keeps its own range:
  // switching H ↔ W restores what was last entered there, or the aspect-ratio
  // equivalent of the other axis's range the first time.
  const rangesByAxis = useRef<Partial<Record<ScanAxisKind, ScanRange>>>({});
  const firstSource = includedSamples
    .map((sample) => sourcesById[sample.sourceId])
    .find((source) => source?.width && source?.height);
  const widthPerHeight =
    firstSource?.width && firstSource.height ? firstSource.width / firstSource.height : null;
  const widthPerHeightRef = useRef(widthPerHeight);
  widthPerHeightRef.current = widthPerHeight;

  const patch = useCallback((partial: Partial<HeightDraft>) => {
    setDraft((current) => {
      let next = { ...current, ...partial };
      const fromAxis = scanAxisKind(current.axisMode);
      const toAxis = scanAxisKind(next.axisMode);
      if (fromAxis !== toAxis) {
        rangesByAxis.current[fromAxis] = {
          start: current.start,
          stop: current.stop,
          refineSelected: current.refineSelected,
        };
        const ratio = widthPerHeightRef.current;
        const restored = rangesByAxis.current[toAxis]
          ?? (ratio
            ? convertScanRange(
                rangesByAxis.current[fromAxis] as ScanRange,
                toAxis === "width" ? ratio : 1 / ratio,
              )
            : null);
        if (restored) next = { ...next, ...restored };
      }
      // Only the transition into a decimal scan picks the even base; later
      // explicit integer choices stay the user's (and are flagged inline).
      return !scansFractionalCandidates(current) && scansFractionalCandidates(next)
        ? withFractionalBase(next)
        : next;
    });
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
