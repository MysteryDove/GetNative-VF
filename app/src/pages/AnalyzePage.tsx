import { useCallback, useEffect, useMemo, useState } from "react";
import type { Translator } from "../i18n";
import type { EngineEnvelope } from "../engine/types";
import { resolveGeometrySnapshot } from "../engine/geometryResolve";
import {
  activateRecipe,
  applyPayloadToCurrentRecipe,
} from "../project/recipeApply";
import { activeRecipe, createRecipe } from "../project/recipe";
import {
  includedSamples as selectIncludedSamples,
  hiddenResultSampleIds as resolveHiddenResultSampleIds,
  selectedAnalysisSamples,
} from "../project/samples";
import { useRunGroupSubmit } from "../hooks/useRunGroupSubmit";
import { useHeightDraft } from "../hooks/useHeightDraft";
import { startHeightRunGroup, type ExecutionBridge } from "../engine/executeRunGroup";
import { KernelAnalyzePanel } from "./KernelAnalyzePanel";
import { defaultKernelDraft, type KernelDraft } from "../engine/kernelDraft";
import {
  buildSeriesTable,
  metricCompatibilityKey,
} from "../engine/runGroupPlan";
import type { ProjectState } from "../project/types";
import { EmptyInlineAction } from "../components/EmptyInlineAction";
import { RecipePicker } from "../components/RecipePicker";
import { RunGroupPlanCard } from "../components/RunGroupPlanCard";
import {
  ApplyGeometryDialog,
  type ApplyGeometryValues,
} from "../components/ApplyGeometryDialog";
import { plotSeriesColor } from "../components/ErrorLinePlot";
import { RecipeSummaryStrip } from "../components/RecipeSummaryStrip";
import { HeightParamsPanel } from "../components/HeightParamsPanel";
import { HeightResultsPanel } from "../components/HeightResultsPanel";
import { Modal } from "../components/Modal";
import { toggleSetValue } from "../utils/collections";
import { srcFromScanSelection } from "../engine/geometry";
import { missingFractionalBaseAxis } from "../engine/heightDraft";
import type { SearchPreset } from "../engine/protocol";

export function AnalyzePage({
  t,
  state,
  capabilities,
  analyzeAvailable,
  subroute,
  initialSampleIds,
  onInitialSampleSelectionConsumed,
  onOpenDiagnostics,
  onOpenSamples,
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
  onOpenSamples: () => void;
  onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
  executionBridge: ExecutionBridge;
}) {
  const [hiddenSampleIds, setHiddenSampleIds] = useState<Set<string>>(() => {
    if (!initialSampleIds?.length) return new Set();
    const selected = new Set(initialSampleIds);
    return new Set(
      selectIncludedSamples(state)
        .filter((sample) => !selected.has(sample.id))
        .map((sample) => sample.id),
    );
  });
  const [applyBusy, setApplyBusy] = useState(false);
  const [applyNotice, setApplyNotice] = useState("");
  const [applyDialogOpen, setApplyDialogOpen] = useState(false);
  const [applySelection, setApplySelection] = useState<string | null>(null);
  const [fractionalWarningAxis, setFractionalWarningAxis] = useState<"height" | "width" | null>(null);
  const [showExcludedResults, setShowExcludedResults] = useState(false);
  const { submitting, notice: submitNotice, submit: submitRunGroup } = useRunGroupSubmit();
  // Kernel draft is lifted here so the hand-built scan list survives subroute
  // switches (height ↔ kernel). ProjectShell keeps route pages mounted so this
  // draft also survives leaving and returning to Analyze during the session.
  const [kernelDraft, setKernelDraft] = useState<KernelDraft | null>(null);
  const [kernelInheritMetric, setKernelInheritMetric] = useState(true);

  const includedSamples = useMemo(
    () => selectIncludedSamples(state),
    [state.samplesById],
  );
  const analysisSamples = useMemo(
    () => selectedAnalysisSamples(includedSamples, initialSampleIds).filter(
      (sample) => !hiddenSampleIds.has(sample.id),
    ),
    [hiddenSampleIds, includedSamples, initialSampleIds],
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
    setHiddenSampleIds(
      new Set(
        includedSamples
          .filter((sample) => !selected.has(sample.id))
          .map((sample) => sample.id),
      ),
    );
    onInitialSampleSelectionConsumed?.();
  }, [includedSamples, initialSampleIds, onInitialSampleSelectionConsumed]);

  const {
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
  } = useHeightDraft({
    capabilities,
    includedSamples: analysisSamples,
    sourcesById: state.sourcesById,
    subroute,
  });

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
    ),
    [heightRuns, state, hiddenResultSampleIds, activeMetricKey, draft.axisMode],
  );

  const visibleSamples = analysisSamples;
  const plotTitle =
    visibleSamples.length === 1
      ? visibleSamples[0].label || visibleSamples[0].id
      : t("analyze.plotTitle");

  const missingBaseAxis = missingFractionalBaseAxis(draft);
  const hasExcludedSamples = Object.values(state.samplesById).some((sample) => !sample.included);

  const runBlockedReason = !analyzeAvailable
    ? t("analyze.runBlocked.noCommand")
    : analysisSamples.length === 0
      ? t("analyze.runBlocked.noSamples")
      : missingBaseAxis
        ? t("analyze.fractionalBaseRequired", {
            base: t(missingBaseAxis === "width" ? "analyze.baseWidth" : "analyze.baseHeight"),
          })
      : !work.ok || !plan
        ? t("analyze.runBlocked.invalidGrid")
        : null;

  const canRun = analyzeAvailable && plan !== null && !submitting;

  function handleSetPreset(preset: SearchPreset) {
    setPreset(preset);
    if (preset !== "fractional_refine") {
      setFractionalWarningAxis(null);
      return;
    }
    const axis = missingFractionalBaseAxis({ ...draft, preset });
    if (axis) setFractionalWarningAxis(axis);
  }

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
            failedNote: result.failed > 0 ? `, ${result.failed} failed` : "",
          }),
        failed: (detail) => t("analyze.submitFailed", { detail }),
      },
    );
  }

  function toggleSampleVisibility(sampleId: string) {
    setHiddenSampleIds((current) => toggleSetValue(current, sampleId));
  }

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

  /** Locale text the apply/naming layer needs. */
  const applyLabels = {
    defaultName: t("recipe.defaultName"),
    unknownKernel: t("recipe.unknownKernel"),
    unknownSize: t("recipe.unknownSize"),
  };

  function openApplyDialog(selected?: string) {
    setApplyNotice("");
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
      const selectedNumber = applySelection == null ? null : Number(applySelection);
      const axis = draft.axisMode;
      const selectedGeometry = selectedNumber != null
        ? srcFromScanSelection({
            axisMode: axis,
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
        profileId: draft.profileId,
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

      <div className="analyze-subroute-host" hidden={subroute !== "kernel"}>
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
            onProjectChange={onProjectChange}
            executionBridge={executionBridge}
          />
        ) : null}
      </div>

      <div className="analyze-subroute-host" hidden={subroute !== "height"}>
        <div className="analyze-layout">
          <aside className="analyze-samples pane">
            <h3>{t("analyze.samplesTitle")}</h3>
            {includedSamples.length === 0 ? (
              <EmptyInlineAction label={t("nav.samples")} onClick={onOpenSamples}>
                <p>{t("analyze.noSamples")}</p>
              </EmptyInlineAction>
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
              <RunGroupPlanCard
                t={t}
                title={t("analyze.runGroupPlan")}
                summary={
                  `${t("analyze.runGroupType", { type: plan.groupType })}` +
                  ` · ${t("analyze.memberCount", { count: String(plan.memberCount) })}`
                }
                workEstimate={t("analyze.workEstimate", { count: String(plan.workEstimate) })}
                members={plan.members.map((member) => ({
                  key: member.planKey,
                  title: member.sampleLabel,
                  subtitle:
                    `${member.kernel.id} · ${member.heightGrid.candidates.length} h · ` +
                    t("analyze.resolvedBase", {
                      width: member.request.baseWidth ?? "integer",
                      height: member.request.baseHeight ?? "integer",
                    }),
                }))}
                truncateAt={12}
                multiMemberNote={
                  plan.memberCount > 1 ? t("analyze.runGroupIsNotSingleRun") : null
                }
              />
            ) : null}
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
            onRefineAroundSelection={refineAroundHeight}
          />

          <HeightParamsPanel
            t={t}
            draft={draft}
            capabilities={capabilities}
            backends={backends}
            kernelCount={kernels.length}
            resolvedBackend={resolvedBackend}
            pNormMaximum={pNormMaximum}
            canRun={canRun}
            submitting={submitting}
            runBlockedReason={runBlockedReason}
            onPatch={patch}
            onSetPreset={handleSetPreset}
            onResetProfileDefaults={resetToProfileDefaults}
            onRun={startRun}
          />
        </div>
      </div>
      {applyDialogOpen && applySourceDims ? (
        <ApplyGeometryDialog
          t={t}
          busy={applyBusy}
          axisMode={draft.axisMode}
          sourceWidth={applySourceDims.width}
          sourceHeight={applySourceDims.height}
          initialSrcHeight={
            applySelection != null && draft.axisMode !== "w_only" ? Number(applySelection) : null
          }
          initialSrcWidth={
            applySelection != null && draft.axisMode === "w_only" ? Number(applySelection) : null
          }
          onCancel={() => setApplyDialogOpen(false)}
          onConfirm={handleApplyGeometry}
        />
      ) : null}
      {fractionalWarningAxis ? (
        <Modal
          onClose={() => setFractionalWarningAxis(null)}
          title={t("analyze.fractionalBaseWarning.title")}
          closeLabel={t("common.close")}
          actions={
            <button
              className="primary-button"
              type="button"
              onClick={() => setFractionalWarningAxis(null)}
            >
              {t("analyze.fractionalBaseWarning.action")}
            </button>
          }
        >
          <p>
            {t("analyze.fractionalBaseWarning.body", {
              base: t(
                fractionalWarningAxis === "width"
                  ? "analyze.baseWidth"
                  : "analyze.baseHeight",
              ),
            })}
          </p>
        </Modal>
      ) : null}
    </div>
  );
}
