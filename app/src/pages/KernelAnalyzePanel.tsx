import { useEffect, useMemo, useState } from "react";
import { SlidersHorizontal } from "lucide-react";
import type { Translator } from "../i18n";
import type { EngineEnvelope } from "../engine/types";
import { kernelDisplayName, profileDisplayName } from "../engine/displayNames";
import { activeRecipe } from "../project/recipe";
import type {
  BackendPreference,
  KernelRef,
  MetricSpec,
} from "../engine/protocol";
import {
  addBicubicGridToScanList,
  addKernelToScanList,
  bicubicRefFromDraft,
  estimateKernelWork,
  geometryGroupKey,
  lanczosRefsFromDraft,
  lanczosTapsRange,
  removeKernelFromScanList,
  resolveKernelCandidates,
  KERNEL_FAMILY_ORDER,
  type KernelDraft,
  type ResolvedGeometryMap,
} from "../engine/kernelDraft";
import { extractKernelResultRows, planKernelRunGroup } from "../engine/kernelRunGroup";
import { metricCompatibilityKey } from "../engine/runGroupPlan";
import { profileFor } from "../engine/profiles";
import {
  kernelSignature,
  resolveBackendPreference,
  selectableBackends,
  validateBackendPNorm,
} from "../engine/heightDraft";
import { startKernelRunGroup, type ExecutionBridge } from "../engine/executeRunGroup";
import { applyPayloadToCurrentRecipe } from "../project/recipeApply";
import { includedSamples as selectIncludedSamples } from "../project/samples";
import { useRunGroupSubmit } from "../hooks/useRunGroupSubmit";
import type { ProjectState, Run } from "../project/types";
import { BlockedState } from "../components/BlockedState";
import { MetricEditor } from "../components/MetricEditor";
import { ResultMetricTable } from "../components/ResultMetricTable";
import { RunGroupPlanCard } from "../components/RunGroupPlanCard";
import { RunLaunchButton } from "../components/RunLaunchButton";
import { backendOptionLabel, pNormMaximumForBackend } from "../engine/backendSelection";
import { toggleSetValue } from "../utils/collections";

type SampleDims = { width: number; height: number };

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
  const [addNotice, setAddNotice] = useState("");
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

  function patch(partial: Partial<KernelDraft>) {
    onDraftChange((current) => ({ ...current, ...partial }));
  }

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

  /** Distinct source shapes whose fixed geometry must be resolved. */
  const geometryGroups = useMemo(() => {
    const groups = new Map<string, { key: string; dims: SampleDims; sampleCount: number }>();
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
      const existing = groups.get(key);
      if (existing) existing.sampleCount += 1;
      else groups.set(key, { key, dims, sampleCount: 1 });
    }
    return [...groups.values()];
  }, [includedSamples, sampleDims, draft.baseHeight, draft.baseWidth, draft.profileId]);

  /** The kernel test is pinned to the current Recipe's geometry. */
  const currentRecipe = activeRecipe(state);
  const recipeGeometry = currentRecipe?.geometry ?? null;
  const recipeProfileId = currentRecipe?.profileId ?? null;

  // Mirror the Recipe geometry's base size (and profile) into the draft so
  // per-source-shape group keys line up with the plan's key derivation.
  useEffect(() => {
    if (!recipeGeometry) return;
    const baseHeight = String(recipeGeometry.baseHeight);
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

  /** Every source shape resolves to the same Recipe geometry. */
  const geometries = useMemo<ResolvedGeometryMap>(() => {
    if (!recipeGeometry) return {};
    return Object.fromEntries(geometryGroups.map((group) => [group.key, recipeGeometry]));
  }, [geometryGroups, recipeGeometry]);

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
    });
    return result.ok ? result.plan : null;
  }, [draft, testSamples, state.sourcesById, geometries, capabilities]);
  const kernelAxisMode = profileFor(draft.profileId, capabilities).default_axis_mode;
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

  const kernelRuns = useMemo(
    () =>
      Object.values(state.runsById)
        .filter((run) => run.runType === "kernel")
        .sort((a, b) => b.createdAt.localeCompare(a.createdAt)),
    [state.runsById],
  );
  const activeMetricKey = metricCompatibilityKey(draft.metric);
  const resultRows = useMemo(
    () => buildKernelResultRows(kernelRuns, state, activeMetricKey),
    [kernelRuns, state, activeMetricKey],
  );
  const kernelTableRows = useMemo(
    () =>
      resultRows.rows.map((row) => {
        const key = `${row.runId}-${row.kernelLabel}`;
        return {
          key,
          metric: row.metric,
          sampleId: row.sampleId,
          cells: [row.kernelLabel, row.sampleLabel, row.runId.slice(0, 10)],
          selected: selectedResultKey === key,
          onSelect: () =>
            setSelectedResultKey((current) => (current === key ? null : key)),
        };
      }),
    [resultRows, selectedResultKey],
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

  const work = candidates.ok
    ? estimateKernelWork({
        sampleCount: testSamples.length,
        candidateCount: candidates.candidates.length,
      })
    : 0;

  const selectedKernel =
    draft.scanList.find((kernel) => kernelSignature(kernel) === selectedSignature) ?? null;

  function handleAddKernels(refs: KernelRef[]) {
    if (!refs.length) {
      setAddNotice(t("analyze.k.scanList.invalidParams"));
      return;
    }
    let addedAny = false;
    onDraftChange((current) => {
      let next = current;
      for (const ref of refs) {
        const result = addKernelToScanList(next, ref);
        next = result.draft;
        addedAny = addedAny || result.added;
      }
      return next;
    });
    setAddNotice(addedAny ? "" : t("analyze.k.scanList.duplicate"));
  }

  function handleAddFamily() {
    if (draft.addFamily === "bicubic") {
      const ref = bicubicRefFromDraft(draft);
      if (!ref) {
        setAddNotice(t("analyze.k.scanList.invalidParams"));
        return;
      }
      handleAddKernels([ref]);
      return;
    }
    if (draft.addFamily === "lanczos") {
      const refs = lanczosRefsFromDraft(draft);
      if (!refs.length) {
        setAddNotice(t("analyze.k.scanList.invalidParams"));
        return;
      }
      handleAddKernels(refs);
      return;
    }
    handleAddKernels([{ id: draft.addFamily, parameters: {} }]);
  }

  function handleAddBicubicGrid() {
    const result = addBicubicGridToScanList(draft);
    if (!result.ok) {
      setAddNotice(t("analyze.k.scanList.invalidParams"));
      return;
    }
    onDraftChange(() => result.draft);
    setAddNotice(
      t("analyze.k.scanList.gridAdded", {
        added: String(result.added),
        skipped: String(result.skipped),
      }),
    );
  }

  function handleRemoveKernel(index: number) {
    onDraftChange((current) => removeKernelFromScanList(current, index));
  }

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

        <div className="analyze-table-host">
          <h3>{t("analyze.k.scanList.title")}</h3>
          {draft.scanList.length ? (
            <div className="candidate-preview" role="table" aria-label={t("analyze.k.scanList.title")}>
              <div className="candidate-preview-meta">
                {t("analyze.k.candidateCount", { count: String(draft.scanList.length) })}
                {work > 0 ? ` · ${t("analyze.workEstimate", { count: String(work) })}` : ""}
              </div>
              <div className="candidate-chips">
                {draft.scanList.map((kernel, index) => {
                  const signature = kernelSignature(kernel);
                  return (
                    <button
                      type="button"
                      className={`candidate-chip ${selectedSignature === signature ? "selected" : ""}`}
                      key={signature}
                      onClick={() =>
                        setSelectedSignature((current) =>
                          current === signature ? null : signature,
                        )
                      }
                    >
                      {kernelChipLabel(t, kernel)}
                      <span
                        className="chip-remove"
                        role="button"
                        aria-label={t("analyze.k.scanList.remove")}
                        title={t("analyze.k.scanList.remove")}
                        onClick={(event) => {
                          event.stopPropagation();
                          handleRemoveKernel(index);
                        }}
                      >
                        ×
                      </span>
                    </button>
                  );
                })}
              </div>
              <p className="help-copy">{t("analyze.k.sequenceExact")}</p>
              <div className="analyze-table-toolbar">
                <button
                  className="secondary-button"
                  type="button"
                  disabled={!selectedKernel}
                  onClick={() => applyKernelRefToCurrentRecipe(selectedKernel, false)}
                >
                  {t("analyze.k.applyToRecipe")}
                </button>
                {!inheritMetric ? (
                  <button
                    className="secondary-button"
                    type="button"
                    disabled={!selectedKernel}
                    onClick={() => applyKernelRefToCurrentRecipe(selectedKernel, true)}
                  >
                    {t("analyze.k.applyWithDivergedMetric")}
                  </button>
                ) : null}
                {applyNotice || submitNotice ? (
                  <span className="help-copy">{applyNotice || submitNotice}</span>
                ) : null}
              </div>
            </div>
          ) : (
            <p className="empty-copy">{t("analyze.k.scanList.empty")}</p>
          )}
        </div>

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

        <fieldset className="metric-fieldset">
          <legend>{t("analyze.k.scanList.addSection")}</legend>
          <div className="kernel-group-chips">
            {KERNEL_FAMILY_ORDER.map((family) => (
              <button
                key={family}
                type="button"
                className={`candidate-chip ${draft.addFamily === family ? "selected" : ""}`}
                onClick={() => patch({ addFamily: family })}
              >
                {kernelDisplayName(t, family)}
              </button>
            ))}
          </div>

          {draft.addFamily === "bicubic" ? (
            <>
              <div className="metric-grid">
                <label className="block">
                  <span>b</span>
                  <input
                    value={draft.bicubicB}
                    onChange={(event) => patch({ bicubicB: event.target.value })}
                  />
                </label>
                <label className="block">
                  <span>c</span>
                  <input
                    value={draft.bicubicC}
                    onChange={(event) => patch({ bicubicC: event.target.value })}
                  />
                </label>
              </div>
              <button
                className="secondary-button"
                type="button"
                onClick={handleAddFamily}
              >
                {t("analyze.k.scanList.add")}
              </button>
              <details className="grid-range">
                <summary>{t("analyze.k.scanList.gridRanges")}</summary>
                <div className="grid-range-body">
                  {(["b", "c"] as const).map((axis) => (
                    <div className="grid-range-row" key={axis}>
                      <span className="grid-range-axis">{axis}</span>
                      {(
                        [
                          [`${axis}Start`, t("analyze.start")],
                          [`${axis}Stop`, t("analyze.stop")],
                          [`${axis}Step`, t("analyze.step")],
                        ] as const
                      ).map(([key, label]) => (
                        <label key={key}>
                          <span>{label}</span>
                          <input
                            value={draft[key]}
                            onChange={(event) => patch({ [key]: event.target.value })}
                          />
                        </label>
                      ))}
                    </div>
                  ))}
                  <p className="help-copy">{t("analyze.k.gridEndpoints")}</p>
                  <button
                    className="secondary-button"
                    type="button"
                    onClick={handleAddBicubicGrid}
                  >
                    {t("analyze.k.scanList.addGrid")}
                  </button>
                </div>
              </details>
            </>
          ) : null}

          {draft.addFamily === "lanczos" ? (
            <>
              <span className="block-label">{t("analyze.k.lanczosTaps")}</span>
              <div className="kernel-group-chips">
                {Array.from(
                  {
                    length:
                      lanczosTapsRange(capabilities).max - lanczosTapsRange(capabilities).min + 1,
                  },
                  (_, i) => lanczosTapsRange(capabilities).min + i,
                ).map((taps) => (
                  <button
                    key={taps}
                    type="button"
                    className={`candidate-chip ${draft.lanczosTapsSelection.includes(taps) ? "selected" : ""}`}
                    onClick={() =>
                      patch({
                        lanczosTapsSelection: draft.lanczosTapsSelection.includes(taps)
                          ? draft.lanczosTapsSelection.filter((value) => value !== taps)
                          : [...draft.lanczosTapsSelection, taps],
                      })
                    }
                  >
                    {taps}
                  </button>
                ))}
              </div>
              <button
                className="secondary-button"
                type="button"
                onClick={handleAddFamily}
              >
                {t("analyze.k.scanList.add")}
              </button>
            </>
          ) : null}

          {draft.addFamily !== "bicubic" && draft.addFamily !== "lanczos" ? (
            <button
              className="secondary-button"
              type="button"
              onClick={handleAddFamily}
            >
              {t("analyze.k.scanList.add")}
            </button>
          ) : null}

          {addNotice ? <p className="help-copy warning-copy">{addNotice}</p> : null}
        </fieldset>

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

function kernelChipLabel(t: Translator, kernel: KernelRef): string {
  const name = kernelDisplayName(t, kernel.id);
  const params = Object.entries(kernel.parameters);
  if (!params.length) return name;
  return `${name} (${params.map(([key, value]) => `${key}=${value}`).join(", ")})`;
}

type KernelResultRow = {
  runId: string;
  sampleId: string;
  kernelId: string;
  parameters: KernelRef["parameters"];
  kernelLabel: string;
  metric: number;
  sampleLabel: string;
};

function buildKernelResultRows(
  runs: Run[],
  state: ProjectState,
  activeMetricKey: string,
): { rows: KernelResultRow[]; incompatibleCount: number } {
  const rows: KernelResultRow[] = [];
  let incompatibleCount = 0;
  for (const run of runs) {
    const snapshot = run.inputSnapshot as {
      metric?: MetricSpec;
    } | null;
    if (snapshot?.metric && metricCompatibilityKey(snapshot.metric) !== activeMetricKey) {
      incompatibleCount += 1;
      continue;
    }
    const extracted = extractKernelResultRows(run.result);
    if (!extracted) continue;
    const sample = run.sampleId ? state.samplesById[run.sampleId] : null;
    for (const row of extracted) {
      const params = Object.entries(row.parameters);
      rows.push({
        runId: run.id,
        sampleId: run.sampleId ?? "",
        kernelId: row.kernelId,
        // Engine echoes the parameters we sent (string | number | boolean).
        parameters: { ...row.parameters } as KernelRef["parameters"],
        kernelLabel: params.length
          ? `${row.kernelId} (${params.map(([key, value]) => `${key}=${value}`).join(", ")})`
          : row.kernelId,
        metric: row.metric,
        sampleLabel: sample?.label ?? run.sampleId ?? "—",
      });
    }
  }
  return { rows, incompatibleCount };
}
