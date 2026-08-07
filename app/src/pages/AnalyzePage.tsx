import { useEffect, useMemo, useState } from "react";
import { Play, SlidersHorizontal } from "lucide-react";
import type { Translator } from "../i18n";
import type { EngineEnvelope } from "../engine/types";
import { kernelDisplayName, profileDisplayName } from "../engine/displayNames";
import { resolveGeometrySnapshot } from "../engine/geometryResolve";
import { applyPayloadToRecipeDraft } from "../project/recipeApply";
import { startHeightRunGroup, type ExecutionBridge } from "../engine/executeRunGroup";
import { KernelAnalyzePanel } from "./KernelAnalyzePanel";
import {
  applyPreset,
  defaultHeightDraft,
  estimateHeightWork,
  fixedKernelsForDraft,
  resolveHeightGrid,
  selectableBackends,
  type HeightDraft,
} from "../engine/heightDraft";
import type { SearchPreset } from "../engine/protocol";
import {
  extractHeightSeries,
  metricCompatibilityKey,
  planHeightRunGroup,
  type HeightRunGroupPlan,
} from "../engine/runGroupPlan";
import type { ProjectState, Run } from "../project/types";
import { BlockedState } from "../components/BlockedState";

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
  const [logDisplay, setLogDisplay] = useState(false);
  const [applyBusy, setApplyBusy] = useState(false);
  const [applyNotice, setApplyNotice] = useState("");
  const [submitting, setSubmitting] = useState(false);

  useEffect(() => {
    if (draftSeeded || !capabilities) return;
    setDraft(defaultHeightDraft(capabilities));
    setDraftSeeded(true);
  }, [capabilities, draftSeeded]);

  const includedSamples = useMemo(
    () =>
      Object.values(state.samplesById)
        .filter((sample) => sample.included)
        .sort((a, b) => a.order - b.order),
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
    () => buildSeriesTable(heightRuns, state, hiddenSampleIds, activeMetricKey, logDisplay),
    [heightRuns, state, hiddenSampleIds, activeMetricKey, logDisplay],
  );

  const runBlockedReason = !analyzeAvailable
    ? t("analyze.runBlocked.noCommand")
    : includedSamples.length === 0
      ? t("analyze.runBlocked.noSamples")
      : !work.ok || !plan
        ? t("analyze.runBlocked.invalidGrid")
        : null;

  const canEdit = true;
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
    setHiddenSampleIds((current) => {
      const next = new Set(current);
      if (next.has(sampleId)) next.delete(sampleId);
      else next.add(sampleId);
      return next;
    });
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

  /**
   * Apply the selected height as the Recipe Draft geometry: resolve the real
   * geometry through the engine geometry command (first included Sample's
   * source shape), then store geometry + MetricSpec + profile into the draft.
   */
  async function applyGeometryToRecipeDraft() {
    if (!selectedHeight || applyBusy) return;
    const baseHeight = Number(selectedHeight);
    if (!Number.isFinite(baseHeight) || baseHeight <= 0) return;
    const sample = includedSamples.find((item) => {
      const source = state.sourcesById[item.sourceId];
      return source?.width && source?.height;
    });
    const source = sample ? state.sourcesById[sample.sourceId] : null;
    if (!sample || !source?.width || !source.height) {
      setApplyNotice(t("recipe.applyNoDims"));
      return;
    }
    setApplyBusy(true);
    setApplyNotice("");
    try {
      const geometry = await resolveGeometrySnapshot({
        profileId: draft.profileId,
        sourceWidth: source.width,
        sourceHeight: source.height,
        baseHeight,
      });
      const result = applyPayloadToRecipeDraft(
        state,
        {
          geometry,
          metric: { ...draft.metric },
          profileId: draft.profileId,
          mathMode: draft.mathMode,
        },
        t("recipe.defaultName"),
      );
      if (!result.ok) {
        setApplyNotice(t("recipe.applyFailed"));
        return;
      }
      const next = result.state;
      onProjectChange(() => next);
      setApplyNotice(t("recipe.applied", { name: result.recipe.name }));
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

      {draft.subroute === "kernel" ? (
        <KernelAnalyzePanel
          t={t}
          state={state}
          capabilities={capabilities}
          analyzeAvailable={analyzeAvailable}
          inheritedMetric={draft.metric}
          inheritedProfileId={draft.profileId}
          inheritedMathMode={draft.mathMode}
          inheritedBackend={draft.backendPreference}
          onOpenDiagnostics={onOpenDiagnostics}
          onProjectChange={onProjectChange}
        />
      ) : (
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
                      <span className="swatch" style={{ background: seriesColor(index) }} />
                      <div>
                        <strong>{sample.label || sample.id}</strong>
                        <span>
                          {source?.label || source?.path || sample.sourceId}
                          {sample.frameIndex != null ? ` · #${sample.frameIndex}` : ""}
                        </span>
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
            <div className="analyze-plot-host">
              {seriesRows.points.length > 0 ? (
                <div className="plot-skeleton" aria-label={t("analyze.plotTitle")}>
                  <div className="plot-skeleton-bars">
                    {seriesRows.points.slice(0, 40).map((point) => (
                      <div
                        key={`${point.runId}-${point.height}`}
                        className={`plot-bar ${selectedHeight === point.height ? "selected" : ""}`}
                        style={{ height: `${Math.max(4, point.displayNormalized * 100)}%` }}
                        title={`${point.height}: ${point.displayValue}`}
                        onClick={() => setSelectedHeight(point.height)}
                      />
                    ))}
                  </div>
                  <p className="help-copy">{t("analyze.plotFromRealRuns")}</p>
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
              <div className="analyze-table-toolbar">
                <h3>{t("analyze.resultsTable")}</h3>
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
                  disabled={!selectedHeight || applyBusy || includedSamples.length === 0}
                  onClick={applyGeometryToRecipeDraft}
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
              {seriesRows.rows.length ? (
                <div className="result-table" role="table" aria-label={t("analyze.resultsTable")}>
                  <div className="result-table-head" role="row">
                    <span role="columnheader">{t("analyze.col.height")}</span>
                    <span role="columnheader">{t("analyze.col.metric")}</span>
                    <span role="columnheader">{t("analyze.col.sample")}</span>
                    <span role="columnheader">{t("analyze.col.kernel")}</span>
                    <span role="columnheader">{t("analyze.col.run")}</span>
                  </div>
                  {seriesRows.rows.slice(0, 200).map((row) => (
                    <button
                      type="button"
                      className={`result-table-row ${selectedHeight === row.height ? "selected" : ""}`}
                      role="row"
                      key={`${row.runId}-${row.height}`}
                      onClick={() => setSelectedHeight(row.height)}
                    >
                      <span role="cell">{row.height}</span>
                      <span role="cell">{row.displayValue}</span>
                      <span role="cell">{row.sampleLabel}</span>
                      <span role="cell">{row.kernelId}</span>
                      <span role="cell">{row.runId.slice(0, 10)}</span>
                    </button>
                  ))}
                </div>
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
            </div>
          </section>

          <aside className="analyze-params pane">
            <h3>
              <SlidersHorizontal size={15} />
              {t("analyze.paramsTitle")}
            </h3>

            <label className="block">
              <span>{t("analyze.preset")}</span>
              <select
                value={draft.preset}
                disabled={!canEdit}
                onChange={(event) => setPreset(event.target.value as SearchPreset)}
              >
                <option value="integer_coarse">{t("analyze.preset.integerCoarse")}</option>
                <option value="fractional_refine">{t("analyze.preset.fractionalRefine")}</option>
                <option value="custom">{t("analyze.preset.custom")}</option>
              </select>
            </label>

            <label className="block">
              <span>{t("analyze.axis")}</span>
              <select
                value={draft.axisMode}
                disabled={!canEdit}
                onChange={(event) =>
                  patch({ axisMode: event.target.value as HeightDraft["axisMode"] })
                }
              >
                <option value="h_only">{t("analyze.axis.hOnly")}</option>
                <option value="w_only">{t("analyze.axis.wOnly")}</option>
                <option value="h_plus_w">{t("analyze.axis.hPlusW")}</option>
              </select>
            </label>

            {draft.preset === "fractional_refine" ? (
              <>
                <label className="block">
                  <span>{t("analyze.refineSelected")}</span>
                  <input
                    value={draft.refineSelected}
                    disabled={!canEdit}
                    onChange={(event) => patch({ refineSelected: event.target.value })}
                  />
                </label>
                <label className="block">
                  <span>{t("analyze.refineHalfSpan")}</span>
                  <input
                    value={draft.refineHalfSpan}
                    disabled={!canEdit}
                    onChange={(event) => patch({ refineHalfSpan: event.target.value })}
                  />
                </label>
                <label className="block">
                  <span>{t("analyze.step")}</span>
                  <input
                    value={draft.step}
                    disabled={!canEdit}
                    onChange={(event) => patch({ step: event.target.value })}
                  />
                </label>
                <label className="block">
                  <span>{t("diagnostics.baseH")}</span>
                  <input
                    value={draft.baseHeight}
                    disabled={!canEdit}
                    onChange={(event) => patch({ baseHeight: event.target.value })}
                    placeholder={t("analyze.optional")}
                  />
                </label>
              </>
            ) : (
              <>
                <label className="block">
                  <span>{t("analyze.start")}</span>
                  <input
                    value={draft.start}
                    disabled={!canEdit}
                    onChange={(event) => patch({ start: event.target.value })}
                  />
                </label>
                <label className="block">
                  <span>{t("analyze.stop")}</span>
                  <input
                    value={draft.stop}
                    disabled={!canEdit}
                    onChange={(event) => patch({ stop: event.target.value })}
                  />
                </label>
                <label className="block">
                  <span>{t("analyze.step")}</span>
                  <input
                    value={draft.step}
                    disabled={!canEdit}
                    onChange={(event) => patch({ step: event.target.value })}
                  />
                </label>
              </>
            )}

            <label className="block">
              <span>{t("analyze.fixedKernel")}</span>
              <select
                value={draft.kernelId}
                disabled={!canEdit || kernelOptions.length === 0}
                onChange={(event) => {
                  const kernel = kernelOptions.find((item) => item.id === event.target.value);
                  patch({
                    kernelId: event.target.value,
                    kernelParameters: { ...(kernel?.parameters ?? {}) },
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

            <label className="checkbox-row">
              <input
                type="checkbox"
                checked={draft.compareCommonKernels}
                disabled={!canEdit}
                onChange={(event) => patch({ compareCommonKernels: event.target.checked })}
              />
              <span>{t("analyze.compareCommonKernels")}</span>
            </label>
            {draft.compareCommonKernels ? (
              <p className="help-copy">
                {t("analyze.compareCommonKernelsHelp", { count: String(kernels.length) })}
              </p>
            ) : null}

            <label className="block">
              <span>{t("diagnostics.profile")}</span>
              <select
                value={draft.profileId}
                disabled={!canEdit}
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

            <label className="block">
              <span>{t("analyze.backend")}</span>
              <select
                value={draft.backendPreference}
                disabled={!canEdit}
                onChange={(event) =>
                  patch({
                    backendPreference: event.target.value as HeightDraft["backendPreference"],
                  })
                }
              >
                {backends.map((backend) => (
                  <option key={backend} value={backend}>
                    {backend === "auto"
                      ? t("analyze.backend.auto")
                      : t(`backend.${backend}` as "backend.cpu")}
                  </option>
                ))}
              </select>
            </label>

            <fieldset className="metric-fieldset">
              <legend>{t("analyze.metricSpec")}</legend>
              <div className="metric-grid">
                {(
                  [
                    ["cropLeft", t("analyze.cropLeft")],
                    ["cropRight", t("analyze.cropRight")],
                    ["cropTop", t("analyze.cropTop")],
                    ["cropBottom", t("analyze.cropBottom")],
                  ] as const
                ).map(([key, label]) => (
                  <label key={key} className="block">
                    <span>{label}</span>
                    <input
                      type="number"
                      min={0}
                      step={1}
                      value={draft.metric[key]}
                      disabled={!canEdit}
                      onChange={(event) =>
                        patch({
                          metric: {
                            ...draft.metric,
                            [key]: Number(event.target.value),
                          },
                        })
                      }
                    />
                  </label>
                ))}
              </div>
              <label className="block">
                <span>{t("analyze.pixelExclusion")}</span>
                <input
                  type="number"
                  min={0}
                  step="any"
                  value={draft.metric.pixelExclusionThreshold}
                  disabled={!canEdit}
                  onChange={(event) =>
                    patch({
                      metric: {
                        ...draft.metric,
                        pixelExclusionThreshold: Number(event.target.value),
                      },
                    })
                  }
                />
              </label>
              <label className="block">
                <span>{t("analyze.pNorm")}</span>
                <input
                  type="number"
                  min={1}
                  step={1}
                  value={draft.metric.pNorm}
                  disabled={!canEdit}
                  onChange={(event) =>
                    patch({
                      metric: {
                        ...draft.metric,
                        pNorm: Number(event.target.value),
                      },
                    })
                  }
                />
              </label>
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
    </div>
  );
}

type SeriesTableRow = {
  runId: string;
  height: string;
  metric: number;
  displayValue: string;
  displayNormalized: number;
  sampleLabel: string;
  kernelId: string;
};

function buildSeriesTable(
  runs: Run[],
  state: ProjectState,
  hiddenSampleIds: Set<string>,
  activeMetricKey: string,
  logDisplay: boolean,
): { rows: SeriesTableRow[]; points: SeriesTableRow[]; incompatibleCount: number } {
  const rows: SeriesTableRow[] = [];
  let incompatibleCount = 0;
  for (const run of runs) {
    if (run.sampleId && hiddenSampleIds.has(run.sampleId)) continue;
    const snapshot = run.inputSnapshot as
      | { metric?: { cropLeft: number; cropRight: number; cropTop: number; cropBottom: number; pixelExclusionThreshold: number; pNorm: number }; kernel?: { id: string } }
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
    for (const point of series) {
      const displayMetric = logDisplay ? Math.log10(point.metric + 1e-9) : point.metric;
      rows.push({
        runId: run.id,
        height: point.height,
        metric: point.metric,
        displayValue: Number.isFinite(displayMetric) ? displayMetric.toPrecision(6) : "—",
        displayNormalized: 0,
        sampleLabel: sample?.label ?? run.sampleId ?? "—",
        kernelId,
      });
    }
  }
  const maxAbs = Math.max(1e-12, ...rows.map((row) => Math.abs(logDisplay ? Math.log10(row.metric + 1e-9) : row.metric)));
  const points = rows.map((row) => {
    const value = logDisplay ? Math.log10(row.metric + 1e-9) : row.metric;
    return {
      ...row,
      displayNormalized: Math.min(1, Math.abs(value) / maxAbs),
    };
  });
  return { rows, points, incompatibleCount };
}

function seriesColor(index: number): string {
  const palette = ["#3b82f6", "#22c55e", "#f59e0b", "#a855f7", "#ef4444", "#06b6d4"];
  return palette[index % palette.length];
}
