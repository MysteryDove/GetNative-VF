import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { Eye, EyeOff } from "lucide-react";
import type { Translator } from "../i18n";
import type { EngineEnvelope } from "../engine/types";
import { resolveGeometrySnapshot } from "../engine/geometryResolve";
import {
  activateRecipe,
  applyPayloadToCurrentRecipe,
} from "../project/recipeApply";
import { activeRecipe, createRecipe, deleteRecipeInState } from "../project/recipe";
import {
  includedSamples as selectIncludedSamples,
  hiddenResultSampleIds as resolveHiddenResultSampleIds,
  selectedAnalysisSamples,
  groupBySourceId,
} from "../project/samples";
import { fileName } from "../media/importSources";
import { useRunGroupSubmit } from "../hooks/useRunGroupSubmit";
import { useHeightDraft } from "../hooks/useHeightDraft";
import { startHeightRunGroup, type ExecutionBridge } from "../engine/executeRunGroup";
import { KernelAnalyzePanel } from "./KernelAnalyzePanel";
import { defaultKernelDraft, type KernelDraft } from "../engine/kernelDraft";
import {
  buildSeriesTable,
  heightRunConfig,
  metricCompatibilityKey,
} from "../engine/runGroupPlan";
import { kernelRefLabel } from "../engine/displayNames";
import type { ProjectState } from "../project/types";
import { EmptyInlineAction } from "../components/EmptyInlineAction";
import { RecipePicker } from "../components/RecipePicker";
import {
  ApplyGeometryDialog,
  type ApplyGeometryValues,
} from "../components/ApplyGeometryDialog";
import { RecipeSummaryStrip } from "../components/RecipeSummaryStrip";
import { HeightParamsPanel } from "../components/HeightParamsPanel";
import { analyzeViewState, restoreDraft, type AnalyzeViewState } from "../project/analyzeView";
import { HeightResultsPanel, type HeightSelection } from "../components/HeightResultsPanel";
import { toggleSetValue } from "../utils/collections";
import { removeRunsFromState } from "../project/runHistory";
import { srcFromScanSelection } from "../engine/geometry";
import {
  invalidKernelBlur,
  invalidKernelParameterNames,
  missingFractionalBaseAxis,
} from "../engine/heightDraft";

export function AnalyzePage({
  t,
  state,
  capabilities,
  analyzeAvailable,
  subroute,
  initialSampleIds,
  onInitialSampleSelectionConsumed,
  onOpenDiagnostics,
  onOpenMedia,
  onOpenKernelTest,
  onOpenVerify,
  onProjectChange,
  executionBridge,
}: {
  t: Translator;
  state: ProjectState;
  capabilities: EngineEnvelope | null;
  analyzeAvailable: boolean;
  /** Owned by the shell nav sidebar (resolution test / algorithm test). */
  subroute: "height" | "kernel";
  initialSampleIds?: readonly string[] | null;
  onInitialSampleSelectionConsumed?: () => void;
  onOpenDiagnostics: () => void;
  onOpenMedia: () => void;
  /** Workflow handoffs offered right after a successful apply. */
  onOpenKernelTest: () => void;
  onOpenVerify: () => void;
  onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
  executionBridge: ExecutionBridge;
}) {
  const unselectedInitialSampleIds = () => {
    if (!initialSampleIds?.length) return new Set<string>();
    const selected = new Set(initialSampleIds);
    return new Set(
      selectIncludedSamples(state)
        .filter((sample) => !selected.has(sample.id))
        .map((sample) => sample.id),
    );
  };
  /** Samples left out of the next Resolution Test (their stored curves stay). */
  const [excludedSampleIds, setExcludedSampleIds] = useState<Set<string>>(unselectedInitialSampleIds);
  /** Samples whose curves are hidden; display only, never changes what runs. */
  const [hiddenSampleIds, setHiddenSampleIds] = useState<Set<string>>(unselectedInitialSampleIds);
  const [applyBusy, setApplyBusy] = useState(false);
  const [applyNotice, setApplyNotice] = useState("");
  const [applyDialogOpen, setApplyDialogOpen] = useState(false);
  const [applySelection, setApplySelection] = useState<HeightSelection | null>(null);
  /** Last pick on the axis not being scanned; offered as the second src dimension. */
  const [applyOtherAxis, setApplyOtherAxis] = useState<HeightSelection | null>(null);
  /** True once geometry was applied: offers the handoff to the Kernel Search. */
  const [applyDone, setApplyDone] = useState(false);
  const [showExcludedResults, setShowExcludedResults] = useState(false);
  const { submitting, notice: submitNotice, submit: submitRunGroup } = useRunGroupSubmit();
  // Kernel draft is lifted here so the hand-built scan list survives subroute
  // switches (height ↔ kernel). ProjectShell keeps route pages mounted so this
  // draft also survives leaving and returning to Analyze during the session.
  const [kernelDraft, setKernelDraft] = useState<KernelDraft | null>(null);
  const [kernelInheritMetric, setKernelInheritMetric] = useState(true);
  const [visitedSubroute, setVisitedSubroute] = useState<ReadonlySet<"height" | "kernel">>(
    () => new Set([subroute]),
  );
  useEffect(() => {
    setVisitedSubroute((current) => {
      if (current.has(subroute)) return current;
      const next = new Set(current);
      next.add(subroute);
      return next;
    });
  }, [subroute]);

  const includedSamples = useMemo(
    () => selectIncludedSamples(state),
    [state.samplesById],
  );
  const analysisSamples = useMemo(
    () => selectedAnalysisSamples(includedSamples, initialSampleIds).filter(
      (sample) => !excludedSampleIds.has(sample.id),
    ),
    [excludedSampleIds, includedSamples, initialSampleIds],
  );

  const hiddenResultSampleIds = useMemo(() => {
    return resolveHiddenResultSampleIds(
      state.samplesById,
      hiddenSampleIds,
      showExcludedResults,
    );
  }, [hiddenSampleIds, showExcludedResults, state.samplesById]);

  useEffect(() => {
    if (initialSampleIds == null) return;
    const selected = new Set(initialSampleIds);
    const unselected = includedSamples
      .filter((sample) => !selected.has(sample.id))
      .map((sample) => sample.id);
    setExcludedSampleIds(new Set(unselected));
    setHiddenSampleIds(new Set(unselected));
    onInitialSampleSelectionConsumed?.();
  }, [includedSamples, initialSampleIds, onInitialSampleSelectionConsumed]);

  const {
    draft,
    patch,
    setPreset,
    refineAroundHeight,
    grid,
    work,
    backends,
    resolvedBackend,
    pNormMaximum,
    plan,
  } = useHeightDraft({
    capabilities,
    includedSamples: analysisSamples,
    sourcesById: state.sourcesById,
    subroute,
    initialMetric: analyzeViewState(state).metric,
    initialDraft: analyzeViewState(state).heightDraft,
  });

  const persistAnalyzeView = useCallback(
    (partial: Partial<AnalyzeViewState>) => {
      onProjectChange((current) => ({
        ...current,
        uiStateByRoute: {
          ...current.uiStateByRoute,
          analyze: { ...analyzeViewState(current), ...partial },
        },
      }));
    },
    [onProjectChange],
  );

  const handleDraftPatch = useCallback(
    (partial: Parameters<typeof patch>[0]) => {
      patch(partial);
      if (partial.metric) persistAnalyzeView({ metric: partial.metric });
    },
    [patch, persistAnalyzeView],
  );

  // Seed the kernel draft lazily: capabilities can arrive after first render.
  useEffect(() => {
    if (kernelDraft !== null || !capabilities) return;
    setKernelDraft(
      restoreDraft(
        defaultKernelDraft(
          draft.metric,
          draft.profileId,
          draft.mathMode,
          draft.backendPreference,
        ) as unknown as Record<string, unknown>,
        analyzeViewState(state).kernelDraft,
        // The metric has its own stored copy; base sizes follow the Recipe.
        ["metric", "baseHeight", "baseWidth"],
      ) as unknown as KernelDraft,
    );
    // The kernel panel mirrors the current Recipe's geometry base into the
    // draft once mounted, so seeding stays profile/metric-only.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [capabilities, kernelDraft]);

  // Parameter drafts travel with the project: reopening it restores the scan
  // range, base modes, kernels and the hand-built scan list. Writes are
  // debounced so typing does not rewrite the project on every keystroke.
  const storedHeightDraft = useRef("");
  const storedKernelDraft = useRef("");
  useEffect(() => {
    const height = JSON.stringify(draft);
    const kernel = kernelDraft ? JSON.stringify(kernelDraft) : "";
    if (!storedHeightDraft.current) {
      // First pass records the baseline without marking the project dirty.
      storedHeightDraft.current = height;
      storedKernelDraft.current = kernel;
      return;
    }
    if (height === storedHeightDraft.current && kernel === storedKernelDraft.current) return;
    const timer = window.setTimeout(() => {
      storedHeightDraft.current = height;
      storedKernelDraft.current = kernel;
      persistAnalyzeView({
        heightDraft: JSON.parse(height) as Record<string, unknown>,
        ...(kernel ? { kernelDraft: JSON.parse(kernel) as Record<string, unknown> } : {}),
      });
    }, 600);
    return () => window.clearTimeout(timer);
  }, [draft, kernelDraft, persistAnalyzeView]);

  // Stable identity: KernelAnalyzePanel effects depend on this callback.
  const handleKernelDraftChange = useCallback(
    (updater: (current: KernelDraft) => KernelDraft) =>
      setKernelDraft((current) => (current ? updater(current) : current)),
    [],
  );

  const heightRuns = useMemo(
    () =>
      Object.values(state.runsById)
        .filter((run) => run.runType === "height")
        .sort((a, b) => b.createdAt.localeCompare(a.createdAt)),
    [state.runsById],
  );

  const activeMetricKey = metricCompatibilityKey(draft.metric);
  const seriesRows = useMemo(
    () => buildSeriesTable(
      heightRuns,
      state,
      hiddenResultSampleIds,
      activeMetricKey,
      draft.axisMode,
      draft.transfer ?? "none",
    ),
    [heightRuns, state, hiddenResultSampleIds, activeMetricKey, draft.axisMode, draft.transfer],
  );

  const visibleSamples = analysisSamples;
  const plotTitle =
    visibleSamples.length === 1
      ? visibleSamples[0].label || visibleSamples[0].id
      : t("analyze.plotTitle");

  const missingBaseAxis = missingFractionalBaseAxis(draft);
  const invalidKernelParameters = invalidKernelParameterNames(draft.kernelParameters);
  const hasExcludedSamples = Object.values(state.samplesById).some((sample) => !sample.included);

  const runBlockedReason = !analyzeAvailable
    ? t("analyze.runBlocked.noCommand")
    : analysisSamples.length === 0
      ? t("analyze.runBlocked.noSamples")
      : missingBaseAxis
        ? t("analyze.fractionalBaseRequired", {
            base: t(missingBaseAxis === "width" ? "analyze.baseWidth" : "analyze.baseHeight"),
          })
      : invalidKernelParameters.length
        ? t("analyze.kernelParamInvalid", { name: invalidKernelParameters.join(", ") })
      : invalidKernelBlur(draft.kernelParameters)
        ? t("analyze.blurInvalid")
      : !work.ok || !plan
        ? t("analyze.runBlocked.invalidGrid")
        : null;

  const canRun = analyzeAvailable && plan !== null && !submitting
    && invalidKernelParameters.length === 0
    && !invalidKernelBlur(draft.kernelParameters);

  function startRun() {
    if (!plan) return;
    setApplyNotice("");
    void submitRunGroup(
      () =>
        startHeightRunGroup({
          plan,
          state,
          onProjectChange,
          bridge: executionBridge,
          mediaFrameBatch: capabilities?.payload.features?.media_frame_batch === true,
        }),
      {
        submitted: (result) =>
          t("analyze.runSubmitted", {
            submitted: String(result.submitted),
            failedNote: result.failed > 0
              ? t("analyze.runFailedNote", { count: String(result.failed) })
              : "",
          }),
        failed: (detail) => t("analyze.submitFailed", { detail }),
      },
    );
  }

  function toggleSampleVisibility(sampleId: string) {
    setHiddenSampleIds((current) => toggleSetValue(current, sampleId));
  }

  function toggleSampleExcluded(sampleId: string) {
    setExcludedSampleIds((current) => toggleSetValue(current, sampleId));
  }

  /**
   * Settings of the Run that measured the selection. Apply carries these over
   * instead of the live parameter draft, which may have changed since.
   */
  const applyRunConfig = useMemo(() => {
    const run = applySelection?.runId ? state.runsById[applySelection.runId] : null;
    if (!run) return null;
    return heightRunConfig(run, run.runGroupId ? state.runGroupsById[run.runGroupId] : null);
  }, [applySelection, state.runGroupsById, state.runsById]);
  const applyAxisMode = applyRunConfig?.axisMode ?? draft.axisMode;
  const applyOtherRunConfig = useMemo(() => {
    const run = applyOtherAxis?.runId ? state.runsById[applyOtherAxis.runId] : null;
    if (!run) return null;
    return heightRunConfig(run, run.runGroupId ? state.runGroupsById[run.runGroupId] : null);
  }, [applyOtherAxis, state.runGroupsById, state.runsById]);
  const applyOtherValue = applyOtherAxis ? Number(applyOtherAxis.height) : null;

  /** First included Sample's source dimensions; required to resolve geometry. */
  const applySourceDims = useMemo(() => {
    const sample = analysisSamples.find((item) => {
      const source = state.sourcesById[item.sourceId];
      return source?.width && source?.height;
    });
    const source = sample ? state.sourcesById[sample.sourceId] : null;
    return source?.width && source.height
      ? { width: source.width, height: source.height }
      : null;
  }, [analysisSamples, state.sourcesById]);

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
    onProjectChange((current) => activateRecipe(current, recipeId));
  }

  function removeRecipe(recipeId: string) {
    onProjectChange((current) => {
      const result = deleteRecipeInState(current, recipeId);
      return result.ok ? result.state : current;
    });
  }

  /** Locale text the apply/naming layer needs. */
  const applyLabels = {
    defaultName: t("recipe.defaultName"),
    unknownKernel: t("recipe.unknownKernel"),
    unknownSize: t("recipe.unknownSize"),
  };

  function openApplyDialog(selected?: HeightSelection, otherAxis?: HeightSelection) {
    // A remembered pick is only offered while the Run that measured it still
    // exists; a deleted run must not silently prefill the other axis.
    setApplyOtherAxis(
      otherAxis?.runId && state.runsById[otherAxis.runId] ? otherAxis : null,
    );
    setApplyNotice("");
    setApplyDone(false);
    if (!applySourceDims) {
      setApplyNotice(t("recipe.applyNoDims"));
      return;
    }
    setApplySelection(selected ?? null);
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
      const selectedNumber = applySelection == null ? null : Number(applySelection.height);
      // A single-axis result combined with the other axis is stored as H+W.
      const axis = values.axisMode ?? applyAxisMode;
      const profileId = applyRunConfig?.profileId ?? draft.profileId;
      const selectedGeometry = selectedNumber != null
        ? srcFromScanSelection({
            axisMode: applyAxisMode,
            selected: selectedNumber,
            sourceWidth: dims.width,
            sourceHeight: dims.height,
          })
        : { srcWidth: dims.width, srcHeight: dims.height };
      const srcHeight = values.srcHeight ?? selectedGeometry.srcHeight;
      const srcWidth = values.srcWidth ?? selectedGeometry.srcWidth;
      if (!(srcHeight > 0) || !(srcWidth > 0)) {
        setApplyNotice(t("recipe.applyFailed"));
        return;
      }
      const geometry = await resolveGeometrySnapshot({
        profileId,
        sourceWidth: dims.width,
        sourceHeight: dims.height,
        axisMode: axis,
        srcHeight,
        srcWidth,
        baseHeight: values.baseHeight,
        baseWidth: values.baseWidth,
      });
      const result = applyPayloadToCurrentRecipe(
        state,
        {
          geometry,
          metric: { ...(applyRunConfig?.metric ?? draft.metric) },
          axisMode: axis,
          profileId,
          mathMode: draft.mathMode,
          // The geometry was found under this curve; Check must use it too.
          transfer: applyRunConfig?.transfer ?? draft.transfer ?? "none",
          ...(values.applyKernel && applyRunConfig?.kernel
            ? { kernel: applyRunConfig.kernel }
            : {}),
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
      setApplyDone(true);
      setApplyDialogOpen(false);
    } catch (error) {
      setApplyNotice(String(error));
    } finally {
      setApplyBusy(false);
    }
  }

  return (
    <div className="page-panel analyze-page">
      <div className="recipe-strip analyze-recipe-strip">
        <div className="recipe-strip-cell">
          <div className="editing-target-row">
            <span className="recipe-strip-label">{t("recipe.strip.active")}</span>
            {recipeOptions.length ? (
              <RecipePicker
                t={t}
                value={currentRecipe?.id ?? ""}
                options={recipeOptions}
                onChange={changeCurrentRecipe}
                onRemove={removeRecipe}
                ariaLabel={t("recipe.strip.active")}
              />
            ) : null}
            <button className="secondary-button" type="button" onClick={createNewRecipe}>
              {t("recipe.newDraft")}
            </button>
          </div>
          <RecipeSummaryStrip
            t={t}
            recipe={currentRecipe}
            activeRecipeId={state.project.activeRecipeId}
            emptyLabel={t("recipe.strip.noActive")}
          />
        </div>
      </div>

      <div className="analyze-subroute-stack">
      <div className={`analyze-subroute-host${subroute === "kernel" ? " is-active" : ""}${visitedSubroute.has("kernel") ? " was-visited" : ""}`} aria-hidden={subroute !== "kernel"}>
        {kernelDraft ? (
          <KernelAnalyzePanel
            t={t}
            state={state}
            capabilities={capabilities}
            analyzeAvailable={analyzeAvailable}
            showExcludedResults={showExcludedResults}
            excludedResultsAvailable={hasExcludedSamples}
            onToggleExcludedResults={setShowExcludedResults}
            draft={kernelDraft}
            onDraftChange={handleKernelDraftChange}
            inheritMetric={kernelInheritMetric}
            onInheritMetricChange={setKernelInheritMetric}
            inheritedMetric={draft.metric}
            onOpenDiagnostics={onOpenDiagnostics}
            onOpenVerify={onOpenVerify}
            onProjectChange={onProjectChange}
            executionBridge={executionBridge}
            metricSpecOpen={analyzeViewState(state).metricSpecOpen}
            onMetricSpecOpenChange={(open) => persistAnalyzeView({ metricSpecOpen: open })}
            onPersistMetric={(metric) => persistAnalyzeView({ metric })}
          />
        ) : null}
      </div>

      <div className={`analyze-subroute-host${subroute === "height" ? " is-active" : ""}${visitedSubroute.has("height") ? " was-visited" : ""}`} aria-hidden={subroute !== "height"}>
        <div className="analyze-layout">
          <aside className="analyze-samples pane">
            <h3>{t("analyze.samplesTitle")}</h3>
            {includedSamples.length === 0 ? (
              <EmptyInlineAction label={t("nav.media")} onClick={onOpenMedia}>
                <p>{t("analyze.noSamples")}</p>
              </EmptyInlineAction>
            ) : (
              <ul className="analyze-sample-list sample-tree">
                {groupBySourceId(includedSamples).map((group) => {
                  const source = state.sourcesById[group.sourceId];
                  const sourceLabel = source?.label || (source?.path ? fileName(source.path) : group.sourceId);
                  return (
                    <li className="sample-tree-group" key={group.sourceId}>
                      <div className="sample-tree-source">
                        <strong>{sourceLabel}</strong>
                        <small>{group.items.length}</small>
                      </div>
                      <ul className="sample-tree-frames">
                        {group.items.map((sample) => {
                          const hidden = hiddenSampleIds.has(sample.id);
                          const excluded = excludedSampleIds.has(sample.id);
                          return (
                            <li
                              key={sample.id}
                              className={`height-sample-row${excluded ? " hidden-series" : ""}`}
                            >
                              <label className="kernel-sample-option" title={t("analyze.includeInRun")}>
                                <input
                                  type="checkbox"
                                  className="sample-check"
                                  checked={!excluded}
                                  aria-label={t("analyze.includeInRun")}
                                  onChange={() => toggleSampleExcluded(sample.id)}
                                />
                                <strong>
                                  {sample.frameIndex != null ? `#${sample.frameIndex}` : (sample.label || sample.id)}
                                </strong>
                              </label>
                              <button
                                type="button"
                                className={`sample-eye${hidden ? " is-off" : ""}`}
                                aria-pressed={!hidden}
                                aria-label={t(hidden ? "analyze.showSeries" : "analyze.hideSeries")}
                                title={t(hidden ? "analyze.showSeries" : "analyze.hideSeries")}
                                onClick={() => toggleSampleVisibility(sample.id)}
                              >
                                {hidden ? <EyeOff size={14} /> : <Eye size={14} />}
                              </button>
                            </li>
                          );
                        })}
                      </ul>
                    </li>
                  );
                })}
              </ul>
            )}

          </aside>

          <HeightResultsPanel
            t={t}
            state={state}
            axisMode={draft.axisMode}
            analyzeAvailable={analyzeAvailable}
            seriesRows={seriesRows}
            grid={grid}
            work={work}
            plotTitle={plotTitle}
            hasIncludedSamples={analysisSamples.length > 0}
            applyBusy={applyBusy}
            applyNotice={applyNotice}
            submitNotice={submitNotice}
            showExcludedResults={showExcludedResults}
            excludedResultsAvailable={hasExcludedSamples}
            onToggleExcludedResults={setShowExcludedResults}
            onOpenDiagnostics={onOpenDiagnostics}
            onOpenApplyDialog={openApplyDialog}
            nextStep={
              applyDone
                ? { label: t("analyze.nextStep.kernel"), onClick: onOpenKernelTest }
                : null
            }
            onRefineAroundSelection={refineAroundHeight}
            onDeleteRuns={(runIds) =>
              onProjectChange((current) => removeRunsFromState(current, runIds))
            }

          />

          <HeightParamsPanel
            t={t}
            draft={draft}
            capabilities={capabilities}
            backends={backends}
            resolvedBackend={resolvedBackend}
            pNormMaximum={pNormMaximum}
            canRun={canRun}
            submitting={submitting}
            runBlockedReason={runBlockedReason}
            onPatch={handleDraftPatch}
            onSetPreset={setPreset}
            work={work}
            onRun={startRun}
            metricSpecOpen={analyzeViewState(state).metricSpecOpen}
            onMetricSpecOpenChange={(open) => persistAnalyzeView({ metricSpecOpen: open })}
          />
        </div>
      </div>
      </div>
      {applyDialogOpen && applySourceDims ? (
        <ApplyGeometryDialog
          t={t}
          busy={applyBusy}
          axisMode={applyAxisMode}
          sourceWidth={applySourceDims.width}
          sourceHeight={applySourceDims.height}
          initialSrcHeight={
            applySelection != null && applyAxisMode !== "w_only"
              ? Number(applySelection.height)
              : applyAxisMode === "w_only" ? applyOtherValue : null
          }
          initialSrcWidth={
            applySelection != null && applyAxisMode === "w_only"
              ? Number(applySelection.height)
              : applyAxisMode === "h_only" ? applyOtherValue : null
          }
          initialBaseHeightMode={
            (applyAxisMode === "w_only" ? applyOtherRunConfig : applyRunConfig)?.baseHeightMode
              ?? draft.baseHeightMode
          }
          initialBaseWidthMode={
            (applyAxisMode === "h_only" ? applyOtherRunConfig : applyRunConfig)?.baseWidthMode
              ?? draft.baseWidthMode
          }
          otherAxisPrefilled={applyAxisMode !== "h_plus_w" && applyOtherValue != null}
          kernelLabel={applyRunConfig?.kernel ? kernelRefLabel(t, applyRunConfig.kernel) : null}
          fromRun={applyRunConfig !== null}
          onCancel={() => setApplyDialogOpen(false)}
          onConfirm={handleApplyGeometry}
        />
      ) : null}
    </div>
  );
}
