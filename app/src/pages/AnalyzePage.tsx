import { useCallback, useEffect, useMemo, useState } from "react";
import { ChevronDown, ChevronRight, Play, RotateCcw, SlidersHorizontal } from "lucide-react";
import type { Translator } from "../i18n";
import type { EngineEnvelope } from "../engine/types";
import { kernelDisplayName, profileDisplayName } from "../engine/displayNames";
import { resolveGeometrySnapshot } from "../engine/geometryResolve";
import {
  applyPayloadToCurrentRecipe,
  setRecipeNameSuffix,
} from "../project/recipeApply";
import { activeRecipe, activateRecipeInState, createRecipe } from "../project/recipe";
import { includedSamples as selectIncludedSamples } from "../project/samples";
import { startHeightRunGroup, type ExecutionBridge } from "../engine/executeRunGroup";
import { KernelAnalyzePanel } from "./KernelAnalyzePanel";
import { defaultKernelDraft, type KernelDraft } from "../engine/kernelDraft";
import {
  applyPreset,
  applyProfileDefaults,
  defaultHeightDraft,
  estimateHeightWork,
  fixedKernelsForDraft,
  kernelSignature,
  resolveBackendPreference,
  resolveHeightGrid,
  selectableBackends,
  type HeightDraft,
} from "../engine/heightDraft";
import type { KernelRef, SearchPreset } from "../engine/protocol";
import {
  extractHeightSeries,
  metricCompatibilityKey,
  planHeightRunGroup,
  type HeightRunGroupPlan,
} from "../engine/runGroupPlan";
import type { ProjectState, Run } from "../project/types";
import { BlockedState } from "../components/BlockedState";
import {
  ApplyGeometryDialog,
  type ApplyGeometryValues,
} from "../components/ApplyGeometryDialog";
import { ErrorLinePlot, plotSeriesColor, DEFAULT_SERIES_COLOR, type ErrorPlotDatum } from "../components/ErrorLinePlot";
import { MetricEditor } from "../components/MetricEditor";
import { RecipeSummaryStrip } from "../components/RecipeSummaryStrip";
import {
  PERFECTLY_DESCALE_THRESHOLD,
  ResultMetricTable,
} from "../components/ResultMetricTable";
import { backendOptionLabel, pNormMaximumForBackend } from "../engine/backendSelection";
import { toggleSetValue } from "../utils/collections";

export function AnalyzePage({
  t,
  state,
  capabilities,
  analyzeAvailable,
  onOpenDiagnostics,
  onOpenSamples,
  onProjectChange,
  executionBridge,
}: {
  t: Translator;
  state: ProjectState;
  capabilities: EngineEnvelope | null;
  analyzeAvailable: boolean;
  onOpenDiagnostics: () => void;
  onOpenSamples: () => void;
  onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
  executionBridge: ExecutionBridge;
}) {
  const [draft, setDraft] = useState<HeightDraft>(() => defaultHeightDraft(capabilities));
  const [draftSeeded, setDraftSeeded] = useState(Boolean(capabilities));
  const [hiddenSampleIds, setHiddenSampleIds] = useState<Set<string>>(new Set());
  const [selectedHeight, setSelectedHeight] = useState<string | null>(null);
  const [logDisplay, setLogDisplay] = useState(true);
  // null = auto: expanded while empty (candidate preview), collapsed once results land.
  const [tableCollapsed, setTableCollapsed] = useState<boolean | null>(null);
  const [applyBusy, setApplyBusy] = useState(false);
  const [applyNotice, setApplyNotice] = useState("");
  const [applyDialogOpen, setApplyDialogOpen] = useState(false);
  const [submitting, setSubmitting] = useState(false);
  // Kernel draft is lifted here so the hand-built scan list survives subroute
  // switches (height ↔ kernel). It does NOT survive leaving the Analyze route
  // (uiStateByRoute persistence is out of scope).
  const [kernelDraft, setKernelDraft] = useState<KernelDraft | null>(null);
  const [kernelInheritMetric, setKernelInheritMetric] = useState(true);

  useEffect(() => {
    if (draftSeeded || !capabilities) return;
    setDraft(defaultHeightDraft(capabilities));
    setDraftSeeded(true);
  }, [capabilities, draftSeeded]);

  // Seed the kernel draft lazily: capabilities can arrive after first render.
  useEffect(() => {
    if (kernelDraft !== null || !capabilities) return;
    setKernelDraft(
      defaultKernelDraft(
        draft.metric,
        draft.profileId,
        draft.mathMode,
        draft.backendPreference,
      ),
    );
    // The kernel panel mirrors the current Recipe's geometry base into the
    // draft once mounted, so seeding stays profile/metric-only.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [capabilities, kernelDraft]);

  // Stable identity: KernelAnalyzePanel effects depend on this callback.
  const handleKernelDraftChange = useCallback(
    (updater: (current: KernelDraft) => KernelDraft) =>
      setKernelDraft((current) => (current ? updater(current) : current)),
    [],
  );

  const includedSamples = useMemo(
    () => selectIncludedSamples(state),
    [state.samplesById],
  );

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
  const kernelOptions = capabilities?.payload.kernels ?? [];
  const profileOptions = capabilities?.payload.profiles ?? [];
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
      sourcesById: state.sourcesById,
      capabilities,
    });
  }, [draft, includedSamples, state.sourcesById, capabilities]);

  const plan: HeightRunGroupPlan | null = planResult?.ok ? planResult.plan : null;

  const heightRuns = useMemo(
    () =>
      Object.values(state.runsById)
        .filter((run) => run.runType === "height")
        .sort((a, b) => b.createdAt.localeCompare(a.createdAt)),
    [state.runsById],
  );

  const activeMetricKey = metricCompatibilityKey(draft.metric);
  const seriesRows = useMemo(
    () => buildSeriesTable(heightRuns, state, hiddenSampleIds, activeMetricKey),
    [heightRuns, state, hiddenSampleIds, activeMetricKey],
  );

  const [hiddenRunIds, setHiddenRunIds] = useState<Set<string>>(new Set());
  const [kernelFilter, setKernelFilter] = useState<string | null>(null);

  // One plot series per run (sample × kernel variant), each with a stable color.
  const runSeries = useMemo(
    () =>
      seriesRows.seriesMeta.map((meta, index) => ({
        ...meta,
        color: plotSeriesColor(index),
        label: `${meta.sampleLabel} · ${kernelMetaLabel(t, meta)}`,
      })),
    [seriesRows, t],
  );
  const runColorById = useMemo(
    () => new Map(runSeries.map((series) => [series.runId, series.color])),
    [runSeries],
  );
  const runLabelById = useMemo(
    () => new Map(runSeries.map((series) => [series.runId, series.label])),
    [runSeries],
  );
  const runKernelById = useMemo(
    () =>
      new Map(
        seriesRows.seriesMeta.map((meta) => [meta.runId, kernelMetaLabel(t, meta)]),
      ),
    [seriesRows, t],
  );

  const plotData = useMemo<ErrorPlotDatum[]>(
    () =>
      seriesRows.points
        .filter((point) => !hiddenRunIds.has(point.runId))
        .map((point) => ({
          key: `${point.runId}-${point.height}`,
          runId: point.runId,
          x: point.height,
          metric: point.metric,
          color: runColorById.get(point.runId) ?? DEFAULT_SERIES_COLOR,
          label: runLabelById.get(point.runId),
        })),
    [seriesRows, hiddenRunIds, runColorById, runLabelById],
  );

  // Reading aids for the getnative workflow: valley (best point) and the
  // perfectly-descale threshold (error <= 1e-6), over the visible series.
  const plotAids = useMemo(() => {
    if (!plotData.length) return null;
    let best = plotData[0];
    for (const point of plotData) if (point.metric < best.metric) best = point;
    const perfectCount = plotData.filter(
      (point) => point.metric <= PERFECTLY_DESCALE_THRESHOLD,
    ).length;
    return {
      bestKey: best.key,
      bestHeight: best.x,
      bestMetric: best.metric,
      bestKernel: runKernelById.get(best.runId) ?? "",
      perfectCount,
    };
  }, [plotData, runKernelById]);

  // Kernel group selector for the result table (only when runs differ by kernel).
  const kernelGroups = useMemo(() => {
    const seen = new Map<string, string>();
    for (const meta of seriesRows.seriesMeta) {
      if (!seen.has(meta.kernelKey)) seen.set(meta.kernelKey, kernelMetaLabel(t, meta));
    }
    return [...seen.entries()].map(([key, label]) => ({ key, label }));
  }, [seriesRows, t]);

  const visibleSamples = includedSamples.filter((sample) => !hiddenSampleIds.has(sample.id));
  const plotTitle =
    visibleSamples.length === 1
      ? visibleSamples[0].label || visibleSamples[0].id
      : t("analyze.plotTitle");

  const filteredRows = useMemo(
    () =>
      kernelFilter
        ? seriesRows.rows.filter((row) => row.kernelKey === kernelFilter)
        : seriesRows.rows,
    [seriesRows, kernelFilter],
  );

  const tableRows = useMemo(
    () =>
      filteredRows.map((row) => ({
        key: `${row.runId}-${row.height}`,
        metric: row.metric,
        cells: [row.height, row.sampleLabel, row.kernelId, row.runId.slice(0, 10)],
        selected: selectedHeight === row.height,
        onSelect: () => setSelectedHeight(row.height),
      })),
    [filteredRows, selectedHeight],
  );

  const isTableCollapsed = tableCollapsed ?? tableRows.length > 0;

  const runBlockedReason = !analyzeAvailable
    ? t("analyze.runBlocked.noCommand")
    : includedSamples.length === 0
      ? t("analyze.runBlocked.noSamples")
      : !work.ok || !plan
        ? t("analyze.runBlocked.invalidGrid")
        : null;

  const canRun = analyzeAvailable && plan !== null && !submitting;

  async function startRun() {
    if (!plan || submitting) return;
    setSubmitting(true);
    setApplyNotice("");
    try {
      const result = await startHeightRunGroup({
        plan,
        state,
        onProjectChange,
        bridge: executionBridge,
        mediaFrameBatch: capabilities?.payload.features?.media_frame_batch === true,
      });
      if (!result.ok) {
        setApplyNotice(t("analyze.submitFailed", { detail: result.reason }));
        return;
      }
      setApplyNotice(
        t("analyze.runSubmitted", {
          submitted: String(result.submitted),
          failedNote: result.failed > 0 ? `, ${result.failed} failed` : "",
        }),
      );
    } catch (error) {
      setApplyNotice(t("analyze.submitFailed", { detail: String(error) }));
    } finally {
      setSubmitting(false);
    }
  }

  function patch(partial: Partial<HeightDraft>) {
    setDraft((current) => ({ ...current, ...partial }));
  }

  function setPreset(preset: SearchPreset) {
    setDraft((current) => applyPreset(current, preset));
  }

  function toggleSampleVisibility(sampleId: string) {
    setHiddenSampleIds((current) => toggleSetValue(current, sampleId));
  }

  function toggleRunVisibility(runId: string) {
    setHiddenRunIds((current) => toggleSetValue(current, runId));
  }

  function applyRefineFromSelection() {
    if (!selectedHeight) return;
    setDraft((current) =>
      applyPreset(
        {
          ...current,
          refineSelected: selectedHeight,
          refineHalfSpan: current.refineHalfSpan || "1.0",
          step: current.step.includes(".") ? current.step : "0.1",
        },
        "fractional_refine",
      ),
    );
  }

  /** First included Sample's source dimensions; required to resolve geometry. */
  const applySourceDims = useMemo(() => {
    const sample = includedSamples.find((item) => {
      const source = state.sourcesById[item.sourceId];
      return source?.width && source?.height;
    });
    const source = sample ? state.sourcesById[sample.sourceId] : null;
    return source?.width && source.height
      ? { width: source.width, height: source.height }
      : null;
  }, [includedSamples, state.sourcesById]);

  /** All Recipes, newest first — the options of the current-recipe selector. */
  const recipeOptions = useMemo(
    () =>
      Object.values(state.recipesById).sort((a, b) =>
        b.updatedAt.localeCompare(a.updatedAt),
      ),
    [state.recipesById],
  );
  const currentRecipe = activeRecipe(state);

  function createNewRecipe() {
    const result = createRecipe(state, {
      name: `${t("recipe.defaultName")} ${Object.keys(state.recipesById).length + 1}`,
    });
    if (!result.ok) return;
    const next = result.state;
    onProjectChange(() => next);
  }

  function changeCurrentRecipe(recipeId: string) {
    const result = activateRecipeInState(state, recipeId);
    if (!result.ok) return;
    const next = result.state;
    onProjectChange(() => next);
  }

  /** Locale text the apply/naming layer needs. */
  const applyLabels = {
    defaultName: t("recipe.defaultName"),
    unknownKernel: t("recipe.unknownKernel"),
    unknownSize: t("recipe.unknownSize"),
  };

  function changeRecipeSuffix(suffix: string) {
    if (!currentRecipe) return;
    const result = setRecipeNameSuffix(state, currentRecipe.id, suffix, applyLabels);
    if (!result.ok) return;
    const next = result.state;
    onProjectChange(() => next);
  }

  function openApplyDialog() {
    setApplyNotice("");
    if (!applySourceDims) {
      setApplyNotice(t("recipe.applyNoDims"));
      return;
    }
    setApplyDialogOpen(true);
  }

  /**
   * Apply the dialog's base height/width as the Recipe Draft geometry:
   * resolve the real geometry through the engine geometry command (deriving
   * the empty side proportionally), then store geometry + MetricSpec +
   * profile into the draft.
   */
  async function handleApplyGeometry(values: ApplyGeometryValues) {
    if (applyBusy) return;
    setApplyBusy(true);
    setApplyNotice("");
    try {
      const dims = applySourceDims;
      if (!dims) {
        setApplyNotice(t("recipe.applyNoDims"));
        return;
      }
      let baseHeight = values.baseHeight;
      if (baseHeight == null && values.baseWidth != null) {
        baseHeight = Math.round((values.baseWidth * dims.height) / dims.width);
      }
      if (baseHeight == null) {
        setApplyNotice(t("recipe.applyFailed"));
        return;
      }
      const geometry = await resolveGeometrySnapshot({
        profileId: draft.profileId,
        sourceWidth: dims.width,
        sourceHeight: dims.height,
        baseHeight,
        baseWidth: values.baseWidth,
      });
      const result = applyPayloadToCurrentRecipe(
        state,
        {
          geometry,
          metric: { ...draft.metric },
          axisMode: draft.axisMode,
          profileId: draft.profileId,
          mathMode: draft.mathMode,
        },
        applyLabels,
      );
      if (!result.ok) {
        setApplyNotice(t("recipe.applyFailed"));
        return;
      }
      const next = result.state;
      onProjectChange(() => next);
      setApplyNotice(t("recipe.applied", { name: result.recipe.name }));
      setApplyDialogOpen(false);
    } catch (error) {
      setApplyNotice(String(error));
    } finally {
      setApplyBusy(false);
    }
  }

  return (
    <div className="page-panel analyze-page">
      <div className="page-header">
        <h2>{t("analyze.title")}</h2>
      </div>

      <div className="recipe-strip">
        <div className="recipe-strip-cell">
          <span className="recipe-strip-label">{t("recipe.strip.active")}</span>
          <div className="editing-target-row">
            {recipeOptions.length ? (
              <select
                value={currentRecipe?.id ?? ""}
                onChange={(event) => {
                  if (event.target.value) changeCurrentRecipe(event.target.value);
                }}
              >
                {!currentRecipe ? (
                  <option value="" disabled>
                    —
                  </option>
                ) : null}
                {recipeOptions.map((recipe) => (
                  <option key={recipe.id} value={recipe.id}>
                    {recipe.name} · {t("recipe.revision", { revision: recipe.revision })}
                  </option>
                ))}
              </select>
            ) : null}
            <button className="secondary-button" type="button" onClick={createNewRecipe}>
              {t("recipe.newDraft")}
            </button>
            {currentRecipe ? (
              <input
                className="recipe-suffix-input"
                value={currentRecipe.nameSuffix ?? ""}
                placeholder={t("analyze.recipeSuffixHint")}
                aria-label={t("analyze.recipeSuffix")}
                title={t("analyze.recipeSuffix")}
                onChange={(event) => changeRecipeSuffix(event.target.value)}
              />
            ) : null}
          </div>
          <RecipeSummaryStrip
            t={t}
            recipe={currentRecipe}
            activeRecipeId={state.project.activeRecipeId}
            emptyLabel={t("recipe.strip.noActive")}
          />
        </div>
      </div>

      <div className="subroute-tabs">
        <button
          className={draft.subroute === "height" ? "active" : ""}
          type="button"
          onClick={() => patch({ subroute: "height" })}
        >
          {t("analyze.height")}
        </button>
        <button
          className={draft.subroute === "kernel" ? "active" : ""}
          type="button"
          onClick={() => patch({ subroute: "kernel" })}
        >
          {t("analyze.kernel")}
        </button>
      </div>

      {draft.subroute === "kernel" && kernelDraft ? (
        <KernelAnalyzePanel
          t={t}
          state={state}
          capabilities={capabilities}
          analyzeAvailable={analyzeAvailable}
          draft={kernelDraft}
          onDraftChange={handleKernelDraftChange}
          inheritMetric={kernelInheritMetric}
          onInheritMetricChange={setKernelInheritMetric}
          inheritedMetric={draft.metric}
          onOpenDiagnostics={onOpenDiagnostics}
          onProjectChange={onProjectChange}
          executionBridge={executionBridge}
        />
      ) : draft.subroute === "kernel" ? null : (
        <div className="analyze-layout">
          <aside className="analyze-samples pane">
            <h3>{t("analyze.samplesTitle")}</h3>
            {includedSamples.length === 0 ? (
              <div className="empty-inline">
                <p>{t("analyze.noSamples")}</p>
                <button className="secondary-button" type="button" onClick={onOpenSamples}>
                  {t("nav.samples")}
                </button>
              </div>
            ) : (
              <ul className="analyze-sample-list">
                {includedSamples.map((sample, index) => {
                  const source = state.sourcesById[sample.sourceId];
                  const hidden = hiddenSampleIds.has(sample.id);
                  return (
                    <li key={sample.id} className={hidden ? "hidden-series" : ""}>
                      <span className="swatch" style={{ background: plotSeriesColor(index) }} />
                      <div>
                        <strong>{sample.label || sample.id}</strong>
                        <span>
                          {source?.label || source?.path || sample.sourceId}
                          {sample.frameIndex != null ? ` · #${sample.frameIndex}` : ""}
                        </span>
                        {sample.tags.length ? (
                          <span className="sample-tags">
                            {sample.tags.map((tag) => (
                              <span className="sample-tag" key={tag}>
                                {tag}
                              </span>
                            ))}
                          </span>
                        ) : null}
                        <label className="series-visibility">
                          <input
                            type="checkbox"
                            checked={!hidden}
                            onChange={() => toggleSampleVisibility(sample.id)}
                          />
                          <span>{t("analyze.seriesVisible")}</span>
                        </label>
                      </div>
                    </li>
                  );
                })}
              </ul>
            )}

            {plan ? (
              <div className="run-group-plan">
                <h3>{t("analyze.runGroupPlan")}</h3>
                <p className="help-copy">
                  {t("analyze.runGroupType", { type: plan.groupType })}
                  {" · "}
                  {t("analyze.memberCount", { count: String(plan.memberCount) })}
                  {" · "}
                  {t("analyze.workEstimate", { count: String(plan.workEstimate) })}
                </p>
                <ul className="run-group-members">
                  {plan.members.slice(0, 12).map((member) => (
                    <li key={member.planKey}>
                      <strong>{member.sampleLabel}</strong>
                      <span>
                        {member.kernel.id}
                        {" · "}
                        {member.heightGrid.candidates.length} h
                      </span>
                    </li>
                  ))}
                  {plan.members.length > 12 ? (
                    <li className="muted">+{plan.members.length - 12}</li>
                  ) : null}
                </ul>
                {plan.memberCount > 1 ? (
                  <p className="help-copy">{t("analyze.runGroupIsNotSingleRun")}</p>
                ) : null}
              </div>
            ) : null}
          </aside>

          <section className="analyze-plot pane">
            <div className="analyze-plot-toolbar">
              <label className="series-visibility">
                <input
                  type="checkbox"
                  checked={logDisplay}
                  onChange={(event) => setLogDisplay(event.target.checked)}
                />
                <span>{t("analyze.logDisplay")}</span>
              </label>
              <button
                className="secondary-button"
                type="button"
                disabled={!selectedHeight}
                onClick={applyRefineFromSelection}
              >
                {t("analyze.refineAroundSelection")}
              </button>
              <button
                className="secondary-button"
                type="button"
                disabled={applyBusy || includedSamples.length === 0}
                onClick={openApplyDialog}
              >
                {t("analyze.applyToRecipe")}
              </button>
              {applyNotice ? <span className="help-copy">{applyNotice}</span> : null}
            </div>
            {seriesRows.incompatibleCount > 0 ? (
              <p className="help-copy warning-copy">
                {t("analyze.incompatibleMetricHidden", {
                  count: String(seriesRows.incompatibleCount),
                })}
              </p>
            ) : null}

            <div className="analyze-plot-host">
              {seriesRows.points.length > 0 ? (
                <div className="plot-skeleton" aria-label={t("analyze.plotTitle")}>
                  {runSeries.length > 1 ? (
                    <div className="plot-legend">
                      {runSeries.map((series) => (
                        <label key={series.runId} className="plot-legend-item">
                          <input
                            type="checkbox"
                            checked={!hiddenRunIds.has(series.runId)}
                            onChange={() => toggleRunVisibility(series.runId)}
                          />
                          <span className="swatch" style={{ background: series.color }} />
                          <span>{series.label}</span>
                        </label>
                      ))}
                    </div>
                  ) : null}
                  <ErrorLinePlot
                    data={plotData}
                    logScale={logDisplay}
                    threshold={PERFECTLY_DESCALE_THRESHOLD}
                    title={plotTitle}
                    xAxisLabel={t("analyze.axisHeight")}
                    yAxisLabel={t("analyze.axisRelativeError")}
                    thresholdLabel={t("analyze.perfectThreshold")}
                    bestKey={plotAids?.bestKey}
                    selectedX={selectedHeight}
                    onSelect={setSelectedHeight}
                    resetLabel={t("plot.resetZoom")}
                  />
                  {plotAids ? (
                    <button
                      type="button"
                      className={`plot-verdict ${plotAids.bestMetric <= PERFECTLY_DESCALE_THRESHOLD ? "perfect" : ""}`}
                      onClick={() => setSelectedHeight(plotAids.bestHeight)}
                      title={t("analyze.verdictSelect")}
                    >
                      <span className="plot-verdict-cell plot-verdict-hero">
                        <span className="plot-verdict-label">{t("analyze.verdict.height")}</span>
                        <strong>{plotAids.bestHeight}</strong>
                      </span>
                      <span className="plot-verdict-cell">
                        <span className="plot-verdict-label">{t("analyze.verdict.kernel")}</span>
                        <span className="plot-verdict-value">{plotAids.bestKernel}</span>
                      </span>
                      <span className="plot-verdict-cell">
                        <span className="plot-verdict-label">{t("analyze.verdict.error")}</span>
                        <span className="plot-verdict-value">
                          {plotAids.bestMetric.toExponential(2)}
                        </span>
                      </span>
                      <span className="plot-verdict-cell">
                        <span className="plot-verdict-label">≤1e-6</span>
                        <span className="plot-verdict-value">
                          {t("analyze.verdict.perfectCount", {
                            count: String(plotAids.perfectCount),
                            total: String(plotData.length),
                          })}
                        </span>
                      </span>
                    </button>
                  ) : null}
                </div>
              ) : (
                <BlockedState
                  title={
                    analyzeAvailable ? t("analyze.noRealRunsTitle") : t("analyze.blockedTitle")
                  }
                  body={
                    analyzeAvailable
                      ? t("analyze.noRealRunsBody")
                      : `${t("analyze.blockedBody")} ${t("analyze.geometryHint")}`
                  }
                  action={
                    <button className="secondary-button" type="button" onClick={onOpenDiagnostics}>
                      {t("nav.diagnostics")}
                    </button>
                  }
                />
              )}
            </div>

            <div className="analyze-table-host">
              <button
                type="button"
                className="analyze-table-toggle"
                aria-expanded={!isTableCollapsed}
                onClick={() => setTableCollapsed(!isTableCollapsed)}
              >
                {isTableCollapsed ? (
                  <ChevronRight size={14} />
                ) : (
                  <ChevronDown size={14} />
                )}
                <span>{t("analyze.resultsTable")}</span>
                <span className="analyze-table-count">{tableRows.length}</span>
              </button>
              {!isTableCollapsed ? (
                <>
                  {kernelGroups.length > 1 ? (
                    <div className="kernel-group-chips">
                      <button
                        type="button"
                        className={`candidate-chip ${kernelFilter === null ? "selected" : ""}`}
                        onClick={() => setKernelFilter(null)}
                      >
                        {t("analyze.allKernelGroups")}
                      </button>
                      {kernelGroups.map((group) => (
                        <button
                          key={group.key}
                          type="button"
                          className={`candidate-chip ${kernelFilter === group.key ? "selected" : ""}`}
                          onClick={() => setKernelFilter(group.key)}
                        >
                          {group.label}
                        </button>
                      ))}
                    </div>
                  ) : null}
                  {tableRows.length ? (
                <ResultMetricTable
                  t={t}
                  ariaLabel={t("analyze.resultsTable")}
                  columns={[
                    t("analyze.col.height"),
                    t("analyze.col.metric"),
                    t("analyze.col.sample"),
                    t("analyze.col.kernel"),
                    t("analyze.col.run"),
                  ]}
                  metricColumnIndex={1}
                  columnTemplate="72px 96px minmax(80px, 1fr) 88px 72px"
                  rows={tableRows}
                />
              ) : (
                <div>
                  <h3>{t("analyze.candidatesTitle")}</h3>
                  {grid.ok ? (
                    <div
                      className="candidate-preview"
                      role="table"
                      aria-label={t("analyze.candidatesTitle")}
                    >
                      <div className="candidate-preview-meta">
                        {t("analyze.candidateCount", {
                          count: String(grid.grid.candidates.length),
                        })}
                        {work.ok
                          ? ` · ${t("analyze.workEstimate", { count: String(work.estimate) })}`
                          : ""}
                      </div>
                      <div className="candidate-chips">
                        {grid.grid.candidates.slice(0, 48).map((value) => (
                          <button
                            type="button"
                            key={value}
                            className={`candidate-chip ${selectedHeight === value ? "selected" : ""}`}
                            onClick={() => setSelectedHeight(value)}
                          >
                            {value}
                          </button>
                        ))}
                        {grid.grid.candidates.length > 48 ? (
                          <span className="candidate-chip muted">
                            +{grid.grid.candidates.length - 48}
                          </span>
                        ) : null}
                      </div>
                    </div>
                  ) : (
                    <p className="help-copy">{t("analyze.invalidGrid")}</p>
                  )}
                </div>
              )}
                </>
              ) : null}
            </div>
          </section>

          <aside className="analyze-params pane">
            <h3>
              <SlidersHorizontal size={15} />
              {t("analyze.paramsTitle")}
            </h3>

            <div className="block">
              <span>{t("analyze.preset")}</span>
              <div className="button-radio" role="radiogroup" aria-label={t("analyze.preset")}>
                <button
                  type="button"
                  role="radio"
                  aria-checked={draft.preset === "integer_coarse"}
                  className={draft.preset === "integer_coarse" ? "active" : ""}
                  onClick={() => setPreset("integer_coarse")}
                >
                  {t("analyze.preset.integerCoarse")}
                </button>
                <button
                  type="button"
                  role="radio"
                  aria-checked={draft.preset === "fractional_refine"}
                  className={draft.preset === "fractional_refine" ? "active" : ""}
                  onClick={() => setPreset("fractional_refine")}
                >
                  {t("analyze.preset.fractionalRefine")}
                </button>
              </div>
            </div>

            <div className="block">
              <span>{t("analyze.axis")}</span>
              <div className="button-radio" role="radiogroup" aria-label={t("analyze.axis")}>
                {(
                  [
                    ["h_plus_w", "analyze.axis.hPlusW"],
                    ["h_only", "analyze.axis.hOnly"],
                    ["w_only", "analyze.axis.wOnly"],
                  ] as const
                ).map(([mode, key]) => (
                  <button
                    key={mode}
                    type="button"
                    role="radio"
                    aria-checked={draft.axisMode === mode}
                    className={draft.axisMode === mode ? "active" : ""}
                    onClick={() => patch({ axisMode: mode })}
                  >
                    {t(key)}
                  </button>
                ))}
              </div>
            </div>

            {draft.preset === "fractional_refine" ? (
              <>
                <label className="block">
                  <span>{t("analyze.refineSelected")}</span>
                  <input
                    value={draft.refineSelected}
                    onChange={(event) => patch({ refineSelected: event.target.value })}
                  />
                </label>
                <label className="block">
                  <span>{t("analyze.refineHalfSpan")}</span>
                  <input
                    value={draft.refineHalfSpan}
                    onChange={(event) => patch({ refineHalfSpan: event.target.value })}
                  />
                </label>
                <label className="block">
                  <span>{t("analyze.step")}</span>
                  <input
                    type="number"
                    min="0.01"
                    step="0.01"
                    value={draft.step}
                    onChange={(event) => patch({ step: event.target.value })}
                  />
                </label>
              </>
            ) : (
              <>
                <label className="block">
                  <span>{t("analyze.start")}</span>
                  <input
                    value={draft.start}
                    onChange={(event) => patch({ start: event.target.value })}
                  />
                </label>
                <label className="block">
                  <span>{t("analyze.stop")}</span>
                  <input
                    value={draft.stop}
                    onChange={(event) => patch({ stop: event.target.value })}
                  />
                </label>
                <label className="block">
                  <span>{t("analyze.step")}</span>
                  <input
                    type="number"
                    min="0.01"
                    step="0.01"
                    value={draft.step}
                    onChange={(event) => patch({ step: event.target.value })}
                  />
                </label>
              </>
            )}

            <label className="block">
              <span>{t("analyze.endpointRule")}</span>
              <select
                value={draft.endpointRule}
                onChange={(event) =>
                  patch({ endpointRule: event.target.value as HeightDraft["endpointRule"] })
                }
              >
                <option value="inclusive">{t("analyze.endpoint.inclusive")}</option>
                <option value="exclusive_stop">{t("analyze.endpoint.exclusive")}</option>
              </select>
            </label>

            <label className="block">
              <span>{t("analyze.baseHeight")}</span>
              <input
                value={draft.baseHeight}
                onChange={(event) => patch({ baseHeight: event.target.value })}
                placeholder={t("analyze.optional")}
              />
            </label>
            <label className="block">
              <span>{t("analyze.baseWidth")}</span>
              <input
                value={draft.baseWidth}
                onChange={(event) => patch({ baseWidth: event.target.value })}
                placeholder={t("analyze.optional")}
              />
            </label>
            <p className="help-copy">{t("analyze.baseParityHint")}</p>

            <label className="block">
              <span>{t("analyze.fixedKernel")}</span>
              <select
                value={draft.kernelId}
                disabled={kernelOptions.length === 0}
                onChange={(event) => {
                  patch({
                    kernelId: event.target.value,
                    kernelParameters:
                      event.target.value === "bicubic"
                        ? { b: 0, c: 0.5 }
                        : event.target.value === "lanczos"
                          ? { taps: 3 }
                          : {},
                    compareKernels: draft.compareKernels.filter(
                      (item) => item.id !== event.target.value,
                    ),
                  });
                }}
              >
                {kernelOptions.length === 0 ? (
                  <option value={draft.kernelId}>{draft.kernelId}</option>
                ) : (
                  kernelOptions.map((kernel) => (
                    <option key={kernel.id} value={kernel.id}>
                      {kernelDisplayName(t, kernel.id)}
                    </option>
                  ))
                )}
              </select>
            </label>

            {draft.kernelId === "bicubic" ? (
              <div className="metric-grid">
                {(["b", "c"] as const).map((parameter) => (
                  <label className="block" key={parameter}>
                    <span>{`Bicubic ${parameter}`}</span>
                    <input
                      type="number"
                      step="any"
                      value={String(draft.kernelParameters[parameter] ?? (parameter === "b" ? 0 : 0.5))}
                      onChange={(event) =>
                        patch({
                          kernelParameters: {
                            ...draft.kernelParameters,
                            [parameter]: event.target.value,
                          },
                        })
                      }
                    />
                  </label>
                ))}
              </div>
            ) : null}
            {draft.kernelId === "lanczos" ? (
              <label className="block">
                <span>{t("analyze.lanczosTaps")}</span>
                <input
                  type="number"
                  min={1}
                  max={15}
                  step={1}
                  value={String(draft.kernelParameters.taps ?? 3)}
                  onChange={(event) =>
                    patch({
                      kernelParameters: { ...draft.kernelParameters, taps: event.target.value },
                    })
                  }
                />
              </label>
            ) : null}

            {kernelOptions.length > 1 ? (
              <fieldset className="metric-fieldset kernel-compare-fieldset">
                <legend>{t("analyze.compareKernels")}</legend>
                <div className="kernel-compare-list">
                  {kernelOptions
                    .filter((kernel) => kernel.id !== draft.kernelId)
                    .flatMap((kernel) => {
                      // Kernels with a taps parameter (lanczos) are offered per-taps.
                      const variants: Array<number | null> =
                        "taps" in kernel.parameters ? [2, 3, 4, 5, 6] : [null];
                      return variants.map((taps) => {
                        const candidate: KernelRef = {
                          id: kernel.id,
                          parameters:
                            taps != null
                              ? { ...kernel.parameters, taps }
                              : { ...kernel.parameters },
                        };
                        const signature = kernelSignature(candidate);
                        const checked = draft.compareKernels.some(
                          (item) => kernelSignature(item) === signature,
                        );
                        const name = kernelDisplayName(t, kernel.id);
                        return (
                          <label
                            key={taps != null ? `${kernel.id}@${taps}` : kernel.id}
                            className="checkbox-row"
                          >
                            <input
                              type="checkbox"
                              checked={checked}
                              onChange={(event) =>
                                patch({
                                  compareKernels: event.target.checked
                                    ? [...draft.compareKernels, candidate]
                                    : draft.compareKernels.filter(
                                        (item) => kernelSignature(item) !== signature,
                                      ),
                                })
                              }
                            />
                            <span>{taps != null ? `${name} ${taps}` : name}</span>
                          </label>
                        );
                      });
                    })}
                </div>
                {draft.compareKernels.length > 0 ? (
                  <p className="help-copy">
                    {t("analyze.compareKernelsHelp", { count: String(kernels.length) })}
                  </p>
                ) : null}
              </fieldset>
            ) : null}

            <label className="block">
              <span>{t("diagnostics.profile")}</span>
              <select
                value={draft.profileId}
                onChange={(event) => patch({ profileId: event.target.value })}
              >
                {profileOptions.length === 0 ? (
                  <option value={draft.profileId}>{draft.profileId}</option>
                ) : (
                  profileOptions.map((profile) => (
                    <option key={profile.id} value={profile.id}>
                      {profileDisplayName(t, profile.id)}
                    </option>
                  ))
                )}
              </select>
            </label>
            <button
              className="secondary-button"
              type="button"
              title={t("analyze.applyProfileDefaults")}
              onClick={() => setDraft((current) => applyProfileDefaults(current, capabilities))}
            >
              <RotateCcw size={14} />
              {t("analyze.applyProfileDefaults")}
            </button>

            <label className="block">
              <span>{t("analyze.backend")}</span>
              <select
                className="backend-select"
                value={draft.backendPreference}
                title={backendOptionLabel(
                  t,
                  draft.backendPreference,
                  capabilities,
                  draft.metric.pNorm,
                  draft.axisMode,
                )}
                onChange={(event) =>
                  patch({
                    backendPreference: event.target.value as HeightDraft["backendPreference"],
                  })
                }
              >
                {backends.map((backend) => (
                  <option key={backend} value={backend}>
                    {backendOptionLabel(
                      t,
                      backend,
                      capabilities,
                      draft.metric.pNorm,
                      draft.axisMode,
                    )}
                  </option>
                ))}
              </select>
            </label>

            <fieldset className="metric-fieldset">
              <legend>{t("analyze.metricSpec")}</legend>
              <MetricEditor
                t={t}
                metric={draft.metric}
                pNormMaximum={pNormMaximum}
                onChange={(metric) => patch({ metric })}
              />
              {draft.metric.pNorm > pNormMaximum ? (
                <span className="help-copy warning-copy">
                  {t("analyze.pNormUnsupported", { backend: resolvedBackend })}
                </span>
              ) : null}
            </fieldset>

            <div className="analyze-run-block">
              <button
                className="primary-button"
                type="button"
                disabled={!canRun}
                onClick={startRun}
              >
                <Play size={15} />
                {submitting ? t("diagnostics.working") : t("analyze.runHeight")}
              </button>
              {runBlockedReason ? <p className="help-copy">{runBlockedReason}</p> : null}
            </div>
          </aside>
        </div>
      )}
      {applyDialogOpen && applySourceDims ? (
        <ApplyGeometryDialog
          t={t}
          busy={applyBusy}
          onCancel={() => setApplyDialogOpen(false)}
          onConfirm={handleApplyGeometry}
        />
      ) : null}
    </div>
  );
}

type SeriesTableRow = {
  runId: string;
  height: string;
  metric: number;
  sampleId: string;
  sampleLabel: string;
  kernelId: string;
  /** Kernel identity including parameter variant, e.g. "lanczos@3". */
  kernelKey: string;
};

/** Per-run series metadata (one entry per run contributing plot points). */
export type RunSeriesMeta = {
  runId: string;
  sampleId: string;
  sampleLabel: string;
  kernelId: string;
  kernelTaps: number | null;
  kernelKey: string;
};

function buildSeriesTable(
  runs: Run[],
  state: ProjectState,
  hiddenSampleIds: Set<string>,
  activeMetricKey: string,
): {
  rows: SeriesTableRow[];
  points: SeriesTableRow[];
  incompatibleCount: number;
  seriesMeta: RunSeriesMeta[];
} {
  const rows: SeriesTableRow[] = [];
  const seriesMeta: RunSeriesMeta[] = [];
  let incompatibleCount = 0;
  for (const run of runs) {
    if (run.sampleId && hiddenSampleIds.has(run.sampleId)) continue;
    const snapshot = run.inputSnapshot as
      | { metric?: { cropLeft: number; cropRight: number; cropTop: number; cropBottom: number; pixelExclusionThreshold: number; pNorm: number }; kernel?: { id: string; parameters?: Record<string, string | number | boolean> } }
      | null;
    if (snapshot?.metric) {
      if (metricCompatibilityKey(snapshot.metric) !== activeMetricKey) {
        incompatibleCount += 1;
        continue;
      }
    }
    const series = extractHeightSeries(run.result);
    if (!series) continue;
    const sample = run.sampleId ? state.samplesById[run.sampleId] : null;
    const kernelId = snapshot?.kernel?.id ?? "—";
    const rawTaps = Number(snapshot?.kernel?.parameters?.taps);
    const kernelTaps = Number.isFinite(rawTaps) ? rawTaps : null;
    const kernelKey = kernelTaps != null ? `${kernelId}@${kernelTaps}` : kernelId;
    const sampleLabel = sample?.label ?? run.sampleId ?? "—";
    seriesMeta.push({
      runId: run.id,
      sampleId: run.sampleId ?? "",
      sampleLabel,
      kernelId,
      kernelTaps,
      kernelKey,
    });
    for (const point of series) {
      rows.push({
        runId: run.id,
        height: point.height,
        metric: point.metric,
        sampleId: run.sampleId ?? "",
        sampleLabel,
        kernelId,
        kernelKey,
      });
    }
  }
  return { rows, points: rows, incompatibleCount, seriesMeta };
}

function kernelMetaLabel(
  t: Translator,
  meta: { kernelId: string; kernelTaps: number | null },
): string {
  const name = kernelDisplayName(t, meta.kernelId);
  return meta.kernelTaps != null ? `${name} ${meta.kernelTaps}` : name;
}
