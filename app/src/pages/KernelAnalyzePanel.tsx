import { useState } from "react";
import { SlidersHorizontal } from "lucide-react";
import type { Translator } from "../i18n";
import type { EngineEnvelope } from "../engine/types";
import { profileDisplayName } from "../engine/displayNames";
import type {
  BackendPreference,
  KernelRef,
  MetricSpec,
} from "../engine/protocol";
import type { KernelDraft } from "../engine/kernelDraft";
import { profileFor } from "../engine/profiles";
import { kernelSignature, selectableBackends } from "../engine/heightDraft";
import { startKernelRunGroup, type ExecutionBridge } from "../engine/executeRunGroup";
import { applyPayloadToCurrentRecipe } from "../project/recipeApply";
import { useRunGroupSubmit } from "../hooks/useRunGroupSubmit";
import { useKernelPlan } from "../hooks/useKernelPlan";
import type { ProjectState } from "../project/types";
import { BlockedState } from "../components/BlockedState";
import { KernelScanList, KernelScanListBuilder } from "../components/KernelScanList";
import { MetricEditor } from "../components/MetricEditor";
import { ResultMetricTable } from "../components/ResultMetricTable";
import { RunGroupPlanCard } from "../components/RunGroupPlanCard";
import { RunLaunchButton } from "../components/RunLaunchButton";
import { backendOptionLabel } from "../engine/backendSelection";
import { toggleSetValue } from "../utils/collections";

export function KernelAnalyzePanel({
  t,
  state,
  capabilities,
  analyzeAvailable,
  draft,
  onDraftChange,
  inheritMetric,
  onInheritMetricChange,
  inheritedMetric,
  onOpenDiagnostics,
  onProjectChange,
  executionBridge,
}: {
  t: Translator;
  state: ProjectState;
  capabilities: EngineEnvelope | null;
  analyzeAvailable: boolean;
  /** Lifted to AnalyzePage so the hand-built scan list survives subroute switches. */
  draft: KernelDraft;
  onDraftChange: (updater: (current: KernelDraft) => KernelDraft) => void;
  inheritMetric: boolean;
  onInheritMetricChange: (value: boolean) => void;
  inheritedMetric: MetricSpec;
  onOpenDiagnostics: () => void;
  onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
  executionBridge: ExecutionBridge;
}) {
  // Selection is by signature, not index: entries can be removed mid-list.
  const [selectedSignature, setSelectedSignature] = useState<string | null>(null);
  const [applyNotice, setApplyNotice] = useState("");
  const { submitting, notice: submitNotice, submit: submitRunGroup } = useRunGroupSubmit();
  /** Samples excluded from the kernel test (default: every included sample). */
  const [excludedSampleIds, setExcludedSampleIds] = useState<Set<string>>(new Set());
  /** Result table sample switch: null = all samples. */
  const [sampleFilter, setSampleFilter] = useState<string | null>(null);
  /** Selected result row (`${runId}-${kernelLabel}`); its kernel can be applied. */
  const [selectedResultKey, setSelectedResultKey] = useState<string | null>(null);

  function toggleSampleExcluded(sampleId: string) {
    setExcludedSampleIds((current) => toggleSetValue(current, sampleId));
  }

  function patch(partial: Partial<KernelDraft>) {
    onDraftChange((current) => ({ ...current, ...partial }));
  }

  const {
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
  } = useKernelPlan({
    state,
    capabilities,
    draft,
    onDraftChange,
    inheritMetric,
    inheritedMetric,
    excludedSampleIds,
    selectedResultKey,
    sampleFilter,
    onSelectResultKey: setSelectedResultKey,
  });

  const selectedKernel =
    draft.scanList.find((kernel) => kernelSignature(kernel) === selectedSignature) ?? null;

  /** Apply a kernel (id + parameters) to the current Recipe (its geometry is already there). */
  function applyKernelRefToCurrentRecipe(kernel: KernelRef | null, includeDivergedMetric: boolean) {
    if (!kernel) return null;
    const result = applyPayloadToCurrentRecipe(
      state,
      {
        kernel: { id: kernel.id, parameters: { ...kernel.parameters } },
        axisMode: profileFor(draft.profileId, capabilities).default_axis_mode,
        profileId: draft.profileId,
        mathMode: draft.mathMode,
        ...(includeDivergedMetric ? { metric: { ...draft.metric } } : {}),
      },
      {
        defaultName: t("recipe.defaultName"),
        unknownKernel: t("recipe.unknownKernel"),
        unknownSize: t("recipe.unknownSize"),
      },
    );
    if (!result.ok) {
      setApplyNotice(t("recipe.applyFailed"));
      return null;
    }
    const next = result.state;
    onProjectChange(() => next);
    setApplyNotice(t("recipe.applied", { name: result.recipe.name }));
    return result.recipe;
  }

  /** Result-row handoff: the measured kernel becomes the current Recipe's kernel. */
  function applySelectedResultKernel() {
    const row = resultRows.rows.find(
      (item) => `${item.runId}-${item.kernelLabel}` === selectedResultKey,
    );
    if (!row) return;
    applyKernelRefToCurrentRecipe(
      { id: row.kernelId, parameters: { ...row.parameters } },
      !inheritMetric,
    );
  }

  const runBlockedReason = !analyzeAvailable
    ? t("analyze.runBlocked.noCommand")
    : includedSamples.length === 0
      ? t("analyze.runBlocked.noSamples")
      : !recipeGeometry
        ? t("analyze.k.noRecipeGeometry")
        : testSamples.length === 0
          ? t("analyze.k.noneSelected")
          : draft.scanList.length === 0
            ? t("analyze.k.scanList.empty")
            : !candidates.ok
              ? t("analyze.k.candidatesInvalid")
              : candidates.candidates.length < 2
                ? t("analyze.k.tooFewCandidates")
                : !plan
                  ? t("analyze.k.planInvalid")
                  : null;

  const canRun = analyzeAvailable && recipeGeometry !== null && plan !== null && !submitting;

  function startRun() {
    if (!plan) return;
    setApplyNotice("");
    void submitRunGroup(
      () =>
        startKernelRunGroup({
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

  return (
    <div className="analyze-layout">
      <aside className="analyze-samples pane">
        <h3>{t("analyze.samplesTitle")}</h3>
        {includedSamples.length === 0 ? (
          <p className="empty-copy">{t("analyze.noSamples")}</p>
        ) : (
          <ul className="analyze-sample-list">
            {includedSamples.map((sample) => {
              const source = state.sourcesById[sample.sourceId];
              const dims = sampleDims[sample.id];
              const excluded = excludedSampleIds.has(sample.id);
              return (
                <li key={sample.id} className={excluded ? "hidden-series" : ""}>
                  <input
                    type="checkbox"
                    className="sample-check"
                    checked={!excluded}
                    aria-label={t("samples.include")}
                    onChange={() => toggleSampleExcluded(sample.id)}
                  />
                  <div>
                    <strong>{sample.label || sample.id}</strong>
                    <span>
                      {source?.label || source?.path || sample.sourceId}
                      {sample.frameIndex != null ? ` · #${sample.frameIndex}` : ""}
                      {dims ? ` · ${dims.width}×${dims.height}` : ""}
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
            multiMemberNote={plan.memberCount > 1 ? t("analyze.k.memberPerSample") : null}
          />
        ) : null}
      </aside>

      <section className="analyze-plot pane">
        <div className="analyze-table-host">
          <h3>{t("analyze.k.geometryTitle")}</h3>
          <p className="help-copy">{t("analyze.k.geometryReadOnly")}</p>
          {recipeGeometry ? (
            <div className="dense-table">
              <div className="dense-row">
                <strong>{currentRecipe?.name}</strong>
                <span>
                  {`${t("analyze.k.canvas")} ${recipeGeometry.canvasWidth}×${recipeGeometry.canvasHeight}` +
                    ` · src (${recipeGeometry.srcLeft}, ${recipeGeometry.srcTop}) ` +
                    `${recipeGeometry.srcWidth}×${recipeGeometry.srcHeight}`}
                </span>
              </div>
            </div>
          ) : (
            <p className="empty-copy">{t("analyze.k.noRecipeGeometry")}</p>
          )}
        </div>

        <KernelScanList
          t={t}
          draft={draft}
          work={work}
          selectedSignature={selectedSignature}
          onSelectSignature={setSelectedSignature}
          onDraftChange={onDraftChange}
          selectedKernel={selectedKernel}
          inheritMetric={inheritMetric}
          onApplyKernel={applyKernelRefToCurrentRecipe}
          notice={applyNotice || submitNotice}
        />

        <div className="analyze-table-host">
          <div className="analyze-table-toolbar">
            <h3>{t("analyze.resultsTable")}</h3>
            <button
              className="primary-button"
              type="button"
              disabled={!selectedResultKey}
              onClick={applySelectedResultKernel}
            >
              {t("analyze.k.setRecipeKernel")}
            </button>
          </div>
          {resultSamples.length > 1 ? (
            <div className="kernel-group-chips">
              <button
                type="button"
                className={`candidate-chip ${sampleFilter === null ? "selected" : ""}`}
                onClick={() => setSampleFilter(null)}
              >
                {t("analyze.k.allSamples")}
              </button>
              {resultSamples.map((sample) => (
                <button
                  key={sample.id}
                  type="button"
                  className={`candidate-chip ${sampleFilter === sample.id ? "selected" : ""}`}
                  onClick={() => setSampleFilter(sample.id)}
                >
                  {sample.label}
                </button>
              ))}
            </div>
          ) : null}
          {resultRows.rows.length ? (
            <ResultMetricTable
              t={t}
              ariaLabel={t("analyze.resultsTable")}
              columns={[
                t("analyze.col.kernel"),
                t("analyze.col.metric"),
                t("analyze.col.sample"),
                t("analyze.col.run"),
              ]}
              metricColumnIndex={1}
              columnTemplate="minmax(140px, 1.4fr) 96px minmax(80px, 1fr) 72px"
              rows={visibleTableRows}
            />
          ) : (
            <BlockedState
              title={
                analyzeAvailable ? t("analyze.noRealRunsTitle") : t("analyze.blockedTitle")
              }
              body={
                analyzeAvailable
                  ? t("analyze.k.noRealRunsBody")
                  : t("analyze.blockedBody")
              }
              action={
                analyzeAvailable ? undefined : (
                  <button className="secondary-button" type="button" onClick={onOpenDiagnostics}>
                    {t("nav.diagnostics")}
                  </button>
                )
              }
            />
          )}
          {resultRows.incompatibleCount > 0 ? (
            <p className="help-copy warning-copy">
              {t("analyze.incompatibleMetricHidden", {
                count: String(resultRows.incompatibleCount),
              })}
            </p>
          ) : null}
        </div>
      </section>

      <aside className="analyze-params pane">
        <h3>
          <SlidersHorizontal size={15} />
          {t("analyze.paramsTitle")}
        </h3>

        <KernelScanListBuilder
          t={t}
          draft={draft}
          capabilities={capabilities}
          onDraftChange={onDraftChange}
        />

        <fieldset className="metric-fieldset">
          <legend>{t("analyze.k.geometryParams")}</legend>
          <label className="block">
            <span>{t("diagnostics.profile")}</span>
            <select
              value={draft.profileId}
              onChange={(event) => patch({ profileId: event.target.value })}
            >
              {(capabilities?.payload.profiles ?? []).map((profile) => (
                <option key={profile.id} value={profile.id}>
                  {profileDisplayName(t, profile.id)}
                </option>
              ))}
              {(capabilities?.payload.profiles ?? []).length === 0 ? (
                <option value={draft.profileId}>{draft.profileId}</option>
              ) : null}
            </select>
          </label>
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
                kernelAxisMode,
              )}
              onChange={(event) =>
                patch({ backendPreference: event.target.value as BackendPreference })
              }
            >
              {selectableBackends(capabilities).map((backend) => (
                <option key={backend} value={backend}>
                  {backendOptionLabel(
                    t,
                    backend,
                    capabilities,
                    draft.metric.pNorm,
                    kernelAxisMode,
                  )}
                </option>
              ))}
            </select>
          </label>
        </fieldset>

        <fieldset className="metric-fieldset">
          <legend>{t("analyze.metricSpec")}</legend>
          <label className="checkbox-row">
            <input
              type="checkbox"
              checked={inheritMetric}
              onChange={(event) => onInheritMetricChange(event.target.checked)}
            />
            <span>{t("analyze.k.inheritMetric")}</span>
          </label>
          {inheritMetric ? (
            <p className="help-copy">{t("analyze.k.inheritMetricNote")}</p>
          ) : (
            <>
              <p className="help-copy warning-copy">{t("analyze.k.metricDiverged")}</p>
              <MetricEditor
                t={t}
                metric={draft.metric}
                pNormMaximum={pNormMaximum}
                onChange={(metric) => patch({ metric })}
              />
            </>
          )}
          {!pNormSupported ? (
            <p className="help-copy warning-copy">
              {t("analyze.pNormUnsupported", { backend: resolvedBackend })}
            </p>
          ) : null}
        </fieldset>

        <RunLaunchButton
          t={t}
          disabled={!canRun}
          submitting={submitting}
          label={t("analyze.k.runKernel")}
          blockedReason={runBlockedReason}
          onClick={startRun}
        />
      </aside>
    </div>
  );
}
