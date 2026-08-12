import { useCallback, useEffect, useMemo, useState } from "react";
import type { Translator } from "../i18n";
import type { EngineEnvelope } from "../engine/types";
import { resolveGeometrySnapshot } from "../engine/geometryResolve";
import {
  activateRecipe,
  applyPayloadToCurrentRecipe,
  setRecipeNameSuffix,
} from "../project/recipeApply";
import { activeRecipe, createRecipe } from "../project/recipe";
import { includedSamples as selectIncludedSamples } from "../project/samples";
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
  const [hiddenSampleIds, setHiddenSampleIds] = useState<Set<string>>(new Set());
  const [applyBusy, setApplyBusy] = useState(false);
  const [applyNotice, setApplyNotice] = useState("");
  const [applyDialogOpen, setApplyDialogOpen] = useState(false);
  const { submitting, notice: submitNotice, submit: submitRunGroup } = useRunGroupSubmit();
  // Kernel draft is lifted here so the hand-built scan list survives subroute
  // switches (height ↔ kernel). It does NOT survive leaving the Analyze route
  // (uiStateByRoute persistence is out of scope).
  const [kernelDraft, setKernelDraft] = useState<KernelDraft | null>(null);
  const [kernelInheritMetric, setKernelInheritMetric] = useState(true);

  const includedSamples = useMemo(
    () => selectIncludedSamples(state),
    [state.samplesById],
  );

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
    includedSamples,
    sourcesById: state.sourcesById,
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
    () => buildSeriesTable(heightRuns, state, hiddenSampleIds, activeMetricKey),
    [heightRuns, state, hiddenSampleIds, activeMetricKey],
  );

  const visibleSamples = includedSamples.filter((sample) => !hiddenSampleIds.has(sample.id));
  const plotTitle =
    visibleSamples.length === 1
      ? visibleSamples[0].label || visibleSamples[0].id
      : t("analyze.plotTitle");

  const runBlockedReason = !analyzeAvailable
    ? t("analyze.runBlocked.noCommand")
    : includedSamples.length === 0
      ? t("analyze.runBlocked.noSamples")
      : !work.ok || !plan
        ? t("analyze.runBlocked.invalidGrid")
        : null;

  const canRun = analyzeAvailable && plan !== null && !submitting;

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
    onProjectChange((current) => activateRecipe(current, recipeId));
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
                  subtitle: `${member.kernel.id} · ${member.heightGrid.candidates.length} h`,
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
            analyzeAvailable={analyzeAvailable}
            seriesRows={seriesRows}
            grid={grid}
            work={work}
            plotTitle={plotTitle}
            hasIncludedSamples={includedSamples.length > 0}
            applyBusy={applyBusy}
            applyNotice={applyNotice}
            submitNotice={submitNotice}
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
            onSetPreset={setPreset}
            onResetProfileDefaults={resetToProfileDefaults}
            onRun={startRun}
          />
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
