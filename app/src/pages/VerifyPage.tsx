import { useEffect, useMemo, useRef, useState } from "react";
import { ChevronDown, ChevronRight, ScanSearch } from "lucide-react";
import type { Translator } from "../i18n";
import type { EngineEnvelope } from "../engine/types";
import type { BackendPreference } from "../engine/protocol";
import { activeRecipe, recipeReadiness, recipesByUpdatedAt } from "../project/recipe";
import { activateRecipe } from "../project/recipeApply";
import { buildFrameSample, nextSampleOrder } from "../project/samples";
import { useRunGroupSubmit } from "../hooks/useRunGroupSubmit";
import { RecipeSummaryStrip } from "../components/RecipeSummaryStrip";
import { DEFAULT_SERIES_COLOR, ErrorLinePlot, plotSeriesColor, type ErrorPlotDatum } from "../components/ErrorLinePlot";
import { toggleSetValue } from "../utils/collections";
import { PERFECTLY_DESCALE_THRESHOLD } from "../components/ResultMetricTable";
import { missingFieldLabels } from "../components/RecipeReviewDialog";
import { EmptyInlineAction } from "../components/EmptyInlineAction";
import { RecipePicker } from "../components/RecipePicker";
import { RunGroupPlanCard } from "../components/RunGroupPlanCard";
import { RunLaunchButton } from "../components/RunLaunchButton";
import { backendOptionLabel, verifySelectableBackends } from "../engine/backendSelection";
import {
  defaultVerifyDraft,
  planVerifyRunGroup,
  reconcileReadyVideoSourceIds,
  verificationRuns,
  validVerifyConcurrency,
  type VerifyDraft,
} from "../engine/verifyPlan";
import { startVerifyRunGroup, type VerifyFrameEntry } from "../engine/executeVerify";
import { VerifyLiveFrameBuffer } from "../engine/verifyLiveFrames";
import {
  reviewVerifyFrames,
  storedVerifyFrames,
  verificationRunLabel,
  verifyCoverageDisplay,
  worstVerifyFrameInRange,
} from "../engine/verifyResults";
import type { ExecutionBridge } from "../engine/executeRunGroup";
import type { ProjectRoute, ProjectState, Run } from "../project/types";

/**
 * Whole-video Verification: setup (sources, scope, range, backend) plus the
 * review loop — live coverage, frame-error timeline, Frame Review Threshold /
 * Top N filters over stored metrics (no re-run), and anomaly-to-Sample.
 */
export function VerifyPage({
  t,
  state,
  capabilities,
  analyzeAvailable,
  onNavigate,
  onProjectChange,
  executionBridge,
}: {
  t: Translator;
  state: ProjectState;
  capabilities: EngineEnvelope | null;
  analyzeAvailable: boolean;
  onNavigate: (route: ProjectRoute) => void;
  onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
  executionBridge: ExecutionBridge;
}) {
  const [draft, setDraft] = useState<VerifyDraft>(() => defaultVerifyDraft());
  const { submitting, notice: submitNotice, submit: submitRunGroup } = useRunGroupSubmit();
  const [notice, setNotice] = useState("");
  const [liveFrameRevision, setLiveFrameRevision] = useState(0);
  const [reviewThreshold, setReviewThreshold] = useState("");
  const [topN, setTopN] = useState("20");
  const [logDisplay, setLogDisplay] = useState(true);
  /** Hidden runs stay in the project but leave the overlay plot. */
  const [hiddenRunIds, setHiddenRunIds] = useState<Set<string>>(new Set());
  /** Drag-zoomed frame range on the plot; null = full view. */
  const [zoomRange, setZoomRange] = useState<{ xMin: number; xMax: number } | null>(null);
  /** Result tables default to collapsed; the plot owns the main area. */
  const [expandedRunIds, setExpandedRunIds] = useState<Set<string>>(new Set());
  const mounted = useRef(true);
  const liveFrameBuffer = useRef<VerifyLiveFrameBuffer | null>(null);
  if (liveFrameBuffer.current === null) {
    liveFrameBuffer.current = new VerifyLiveFrameBuffer(() => {
      if (mounted.current) setLiveFrameRevision((current) => current + 1);
    });
  }

  function toggleRunExpanded(runId: string) {
    setExpandedRunIds((current) => toggleSetValue(current, runId));
  }

  function toggleRunVisible(runId: string) {
    setHiddenRunIds((current) => toggleSetValue(current, runId));
  }
  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
      liveFrameBuffer.current?.dispose();
    };
  }, []);

  const liveFrames = useMemo(
    () => liveFrameBuffer.current?.snapshotAll() ?? {},
    [liveFrameRevision],
  );

  const recipe = activeRecipe(state);
  const recipeOptions = useMemo(() => recipesByUpdatedAt(state), [state.recipesById]);
  const recipeGaps = recipe ? recipeReadiness(recipe) : null;
  const readyVideos = useMemo(
    () =>
      Object.values(state.sourcesById).filter(
        (source) => source.kind === "video" && source.state === "ready",
      ),
    [state.sourcesById],
  );
  const readyVideoIds = useMemo(
    () => readyVideos.map((source) => source.id),
    [readyVideos],
  );
  const readyVideoIdsKey = useMemo(
    () => [...readyVideoIds].sort().join("\u0000"),
    [readyVideoIds],
  );

  useEffect(() => {
    setDraft((current) => {
      const sourceIds = reconcileReadyVideoSourceIds(current.sourceIds, readyVideoIds);
      return sourceIds === current.sourceIds ? current : { ...current, sourceIds };
    });
  }, [readyVideoIdsKey]);

  const plan = useMemo(() => {
    if (!recipe) return null;
    const result = planVerifyRunGroup({
      draft,
      recipe,
      sourcesById: state.sourcesById,
    });
    return result.ok ? result.plan : null;
  }, [draft, recipe, state.sourcesById]);

  const runs = useMemo(() => verificationRuns(state), [state]);
  const concurrencyCapability = capabilities?.payload.features?.media_verify_concurrency;
  const concurrencyMin = concurrencyCapability?.min ?? 1;
  const concurrencyMax = concurrencyCapability?.max ?? 8;
  const concurrencyInvalid = !validVerifyConcurrency(draft.concurrency);

  /** Stable per-run colors (indexed over all runs, not the filtered view). */
  const runColorById = useMemo(
    () => new Map(runs.map((run, index) => [run.id, plotSeriesColor(index)])),
    [runs],
  );
  const runLabelById = useMemo(
    () =>
      new Map(
        runs.map((run) => {
          return [run.id, verificationRunLabel(run, state, t)];
        }),
      ),
    [runs, state.sourcesById, state.recipesById, t],
  );
  const plotData = useMemo<ErrorPlotDatum[]>(
    () =>
      runs
        .filter((run) => !hiddenRunIds.has(run.id))
        .flatMap((run) =>
          (storedVerifyFrames(run) ?? liveFrames[run.id] ?? [])
            .filter((frame) => frame.error !== null)
            .map((frame) => ({
              key: `${run.id}-${frame.frameIndex}`,
              runId: run.id,
              x: String(frame.frameIndex),
              metric: frame.error as number,
              color: runColorById.get(run.id) ?? DEFAULT_SERIES_COLOR,
              label: runLabelById.get(run.id),
            })),
        ),
    [runs, hiddenRunIds, liveFrames, runColorById, runLabelById],
  );

  /** Highest-error frame inside the zoomed range, across visible runs. */
  const rangeWorst = useMemo(() => {
    if (!zoomRange) return null;
    let worst: { run: Run; frame: VerifyFrameEntry } | null = null;
    for (const run of runs) {
      if (hiddenRunIds.has(run.id)) continue;
      const frame = worstVerifyFrameInRange(
        storedVerifyFrames(run) ?? liveFrames[run.id] ?? [],
        zoomRange,
      );
      if (frame && (!worst || (frame.error as number) > (worst.frame.error as number))) {
        worst = { run, frame };
      }
    }
    return worst;
  }, [zoomRange, runs, hiddenRunIds, liveFrames]);

  function patch(partial: Partial<VerifyDraft>) {
    setDraft((current) => ({ ...current, ...partial }));
  }

  function toggleSource(sourceId: string) {
    setDraft((current) => ({
      ...current,
      sourceIds: current.sourceIds.includes(sourceId)
        ? current.sourceIds.filter((id) => id !== sourceId)
        : [...current.sourceIds, sourceId],
    }));
  }

  function changeActiveRecipe(recipeId: string) {
    onProjectChange((current) => activateRecipe(current, recipeId));
  }

  const startBlockedReason = !analyzeAvailable
    ? t("verify.blocked.noCommand")
    : !recipe
      ? t("verify.blocked.noRecipe")
      : recipeGaps && !recipeGaps.ok
        ? t("verify.blocked.incomplete", {
            missing: missingFieldLabels(t, recipeGaps.missing),
          })
        : draft.sourceIds.length === 0
          ? t("verify.blocked.noSources")
          : concurrencyInvalid
            ? t("verify.blocked.concurrency")
          : !plan
            ? t("verify.blocked.invalidPlan")
            : null;

  const canStart = analyzeAvailable && plan !== null && !submitting;

  function startRun() {
    if (!plan || !recipe) return;
    setNotice("");
    void submitRunGroup(
      () =>
        startVerifyRunGroup({
          plan,
          recipe,
          state,
          onProjectChange,
          bridge: executionBridge,
          onFrames: (runId, entries) => {
            if (!mounted.current) return;
            liveFrameBuffer.current?.append(runId, entries);
          },
          onTerminal: (runId) => liveFrameBuffer.current?.clear(runId),
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

  function addFrameToSamples(run: Run, entry: VerifyFrameEntry) {
    const source = run.sourceId ? state.sourcesById[run.sourceId] : null;
    if (!source) return;
    const scope = (run.inputSnapshot as { scanScope?: { streamIndex?: number } } | null)
      ?.scanScope;
    const streamIndex = scope?.streamIndex ?? source.selectedStreamIndex ?? 0;
    const duplicate = Object.values(state.samplesById).find(
      (sample) =>
        sample.sourceId === source.id &&
        sample.streamIndex === streamIndex &&
        sample.frameIndex === entry.frameIndex,
    );
    if (duplicate) {
      setNotice(t("verify.sampleExists"));
      return;
    }
    const order = nextSampleOrder(state.samplesById);
    const sample = buildFrameSample({
      source,
      order,
      label: `${source.label || source.path} #${entry.frameIndex}`,
      streamIndex,
      frameIndex: entry.frameIndex,
      pts: entry.pts ?? null,
      bestEffortTimestamp: entry.pts ?? null,
      timestampSeconds: entry.timestampSeconds ?? null,
      originRunId: run.id,
    });
    onProjectChange((current) => ({
      ...current,
      samplesById: { ...current.samplesById, [sample.id]: sample },
    }));
    setNotice(t("verify.sampleAdded", { frame: String(entry.frameIndex) }));
  }

  return (
    <div className="page-panel analyze-page">
      <div className="page-header">
        <h2>{t("verify.title")}</h2>
      </div>

      <section className="page-section verify-recipe-strip">
        <h3>{t("verify.activeRecipe")}</h3>
        {recipeOptions.length > 0 ? (
          <div className="verify-current-row">
            <RecipePicker
              t={t}
              value={state.project.activeRecipeId ?? ""}
              options={recipeOptions}
              onChange={changeActiveRecipe}
              ariaLabel={t("verify.selectRecipe")}
            />
            <RecipeSummaryStrip
              t={t}
              recipe={recipe}
              activeRecipeId={state.project.activeRecipeId}
              emptyLabel={t("verify.noActiveRecipe")}
            />
          </div>
        ) : (
          <>
            <RecipeSummaryStrip
              t={t}
              recipe={recipe}
              activeRecipeId={state.project.activeRecipeId}
              emptyLabel={t("verify.noActiveRecipe")}
            />
            <EmptyInlineAction label={t("nav.analyze")} onClick={() => onNavigate("analyze")}>
              {null}
            </EmptyInlineAction>
          </>
        )}
      </section>

      <div className="analyze-layout">
        <aside className="analyze-samples pane">
          <h3>
            <span>{t("verify.sourcesTitle")}</span>
            {readyVideos.length > 0 ? (
              <span
                className="verify-source-count"
                aria-label={t("verify.sourcesSelected", {
                  selected: String(draft.sourceIds.length),
                  total: String(readyVideos.length),
                })}
              >
                {draft.sourceIds.length}/{readyVideos.length}
              </span>
            ) : null}
          </h3>
          {readyVideos.length === 0 ? (
            <EmptyInlineAction label={t("nav.media")} onClick={() => onNavigate("media")}>
              <p>{t("verify.noVideos")}</p>
            </EmptyInlineAction>
          ) : (
            <ul className="analyze-sample-list verify-source-list">
              {readyVideos.map((source) => {
                const selected = draft.sourceIds.includes(source.id);
                return (
                  <li key={source.id} className={selected ? "selected" : ""}>
                    <label className="verify-source-option">
                      <input
                        type="checkbox"
                        className="sample-check"
                        checked={selected}
                        onChange={() => toggleSource(source.id)}
                      />
                      <div className="verify-source-copy">
                        <strong>{source.label || source.path}</strong>
                        <span>
                          {source.width && source.height
                            ? `${source.width}×${source.height}`
                            : ""}
                          {source.durationSeconds != null
                            ? ` · ${source.durationSeconds.toFixed(1)}s`
                            : ""}
                        </span>
                      </div>
                    </label>
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
              members={plan.members.map((member) => ({
                key: member.planKey,
                title: member.sourceLabel,
                subtitle: scopeLabel(t, member.scanScope.selection),
              }))}
              multiMemberNote={plan.memberCount > 1 ? t("verify.memberPerSource") : null}
            />
          ) : null}
        </aside>

        <section className="analyze-plot pane">
          <div className="verify-review-toolbar">
            <h3>{t("verify.reviewTitle")}</h3>
            {runs.length > 0 ? (
              <div className="verify-review-filters">
                <label className="series-visibility">
                  <input
                    type="checkbox"
                    checked={logDisplay}
                    onChange={(event) => setLogDisplay(event.target.checked)}
                  />
                  <span>{t("analyze.logDisplay")}</span>
                </label>
                <label className="block verify-filter">
                  <span>{t("verify.reviewThreshold")}</span>
                  <input
                    value={reviewThreshold}
                    placeholder={t("verify.thresholdOff")}
                    onChange={(event) => setReviewThreshold(event.target.value)}
                  />
                </label>
                <label className="block verify-filter">
                  <span>{t("verify.topN")}</span>
                  <input
                    value={topN}
                    onChange={(event) => setTopN(event.target.value)}
                  />
                </label>
              </div>
            ) : null}
          </div>
          {runs.length > 1 ? (
            <div className="plot-legend">
              {runs.map((run) => (
                <label key={run.id} className="plot-legend-item">
                  <input
                    type="checkbox"
                    checked={!hiddenRunIds.has(run.id)}
                    onChange={() => toggleRunVisible(run.id)}
                  />
                  <span className="swatch" style={{ background: runColorById.get(run.id) }} />
                  <span>{runLabelById.get(run.id)}</span>
                </label>
              ))}
            </div>
          ) : null}
          {runs.length === 0 ? (
            <div className="verify-review-empty">
              <ScanSearch aria-hidden="true" />
              <p>{t("verify.noRuns")}</p>
            </div>
          ) : (
            <div className="analyze-plot-host">
              {plotData.length ? (
                <ErrorLinePlot
                  data={plotData}
                  logScale={logDisplay}
                  threshold={PERFECTLY_DESCALE_THRESHOLD}
                  title={t("verify.plotTitle")}
                  xAxisLabel={t("verify.col.frame")}
                  yAxisLabel={t("analyze.axisRelativeError")}
                  thresholdLabel={t("analyze.perfectThreshold")}
                  resetLabel={t("plot.resetZoom")}
                  onZoomRangeChange={setZoomRange}
                />
              ) : (
                <p className="empty-copy">{t("verify.noFramesAbove")}</p>
              )}
            </div>
          )}
          {zoomRange ? (
            <div className="verify-range-bar">
              <span className="help-copy">
                {t("verify.zoomRange", {
                  from: String(Math.round(zoomRange.xMin)),
                  to: String(Math.round(zoomRange.xMax)),
                })}
                {rangeWorst
                  ? ` · ${t("verify.zoomWorstFrame", {
                      frame: String(rangeWorst.frame.frameIndex),
                      error: (rangeWorst.frame.error as number).toExponential(2),
                    })}`
                  : ""}
              </span>
              {rangeWorst ? (
                <button
                  className="secondary-button"
                  type="button"
                  onClick={() => addFrameToSamples(rangeWorst.run, rangeWorst.frame)}
                >
                  {t("verify.addToSamples")}
                </button>
              ) : null}
            </div>
          ) : null}
          {runs.length > 0 ? (
            <p className="help-copy">{t("verify.reviewFilterHint")}</p>
          ) : null}
          {notice || submitNotice ? (
            <p className="help-copy">{notice || submitNotice}</p>
          ) : null}
          {runs.map((run) => (
            <VerifyRunReview
              key={run.id}
              t={t}
              run={run}
              state={state}
              live={liveFrames[run.id] ?? null}
              threshold={reviewThreshold}
              topN={topN}
              expanded={expandedRunIds.has(run.id)}
              onToggleExpanded={() => toggleRunExpanded(run.id)}
              onAddFrame={(entry) => addFrameToSamples(run, entry)}
            />
          ))}
        </section>

        <aside className="analyze-params pane">
          <h3>{t("verify.setupTitle")}</h3>

          <fieldset className="metric-fieldset">
            <legend>{t("verify.scope")}</legend>
            <div className="button-radio" role="radiogroup" aria-label={t("verify.scope")}>
              <button
                type="button"
                role="radio"
                aria-checked={draft.scopeKind === "preview"}
                className={draft.scopeKind === "preview" ? "active" : ""}
                onClick={() => patch({ scopeKind: "preview" })}
              >
                {t("verify.scopePreview")}
              </button>
              <button
                type="button"
                role="radio"
                aria-checked={draft.scopeKind === "full"}
                className={draft.scopeKind === "full" ? "active" : ""}
                onClick={() => patch({ scopeKind: "full" })}
              >
                {t("verify.scopeFull")}
              </button>
            </div>
            <p className="help-copy">
              {t(draft.scopeKind === "preview" ? "verify.scopePreviewHint" : "verify.scopeFullHint")}
            </p>
            {draft.scopeKind === "preview" ? (
              <>
                <label className="block">
                  <span>{t("verify.previewRule")}</span>
                  <select
                    value={draft.previewRule}
                    onChange={(event) =>
                      patch({
                        previewRule: event.target.value as VerifyDraft["previewRule"],
                      })
                    }
                  >
                    <option value="decoded_i_picture">{t("verify.ruleIPicture")}</option>
                    <option value="every_n">{t("verify.ruleEveryN")}</option>
                  </select>
                </label>
                {draft.previewRule === "every_n" ? (
                  <label className="block">
                    <span>{t("verify.everyN")}</span>
                    <input
                      value={draft.everyN}
                      onChange={(event) => patch({ everyN: event.target.value })}
                    />
                  </label>
                ) : null}
              </>
            ) : null}
          </fieldset>

          <fieldset className="metric-fieldset">
            <legend>{t("verify.range")}</legend>
            <label className="block">
              <span>{t("verify.startFrame")}</span>
              <input
                value={draft.startFrame}
                placeholder={t("verify.wholeSource")}
                onChange={(event) => patch({ startFrame: event.target.value })}
              />
            </label>
            <label className="block">
              <span>{t("verify.endFrame")}</span>
              <input
                value={draft.endFrame}
                placeholder={t("verify.wholeSource")}
                onChange={(event) => patch({ endFrame: event.target.value })}
              />
            </label>
          </fieldset>

          <label className="block">
            <span>{t("analyze.backend")}</span>
            <select
              className="backend-select"
              value={draft.backendPreference}
              title={backendOptionLabel(
                t,
                draft.backendPreference,
                capabilities,
                recipe?.metric?.pNorm ?? 1,
                undefined,
                capabilities?.payload.features?.verify_engine_decode !== true,
              )}
              onChange={(event) =>
                patch({ backendPreference: event.target.value as BackendPreference })
              }
            >
              {verifySelectableBackends(capabilities).map((backend) => (
                <option key={backend} value={backend}>
                  {backendOptionLabel(
                    t,
                    backend,
                    capabilities,
                    recipe?.metric?.pNorm ?? 1,
                    undefined,
                    capabilities?.payload.features?.verify_engine_decode !== true,
                  )}
                </option>
              ))}
            </select>
          </label>

          <label className="block">
            <span>{t("verify.concurrency")}</span>
            <input
              type="number"
              min={concurrencyMin}
              max={concurrencyMax}
              step={1}
              value={draft.concurrency}
              aria-invalid={concurrencyInvalid}
              onChange={(event) => patch({ concurrency: Number(event.target.value) })}
            />
            {concurrencyInvalid ? (
              <small className="field-error">
                {t("verify.concurrencyInvalid", {
                  min: String(concurrencyMin),
                  max: String(concurrencyMax),
                })}
              </small>
            ) : null}
          </label>

          <RunLaunchButton
            t={t}
            disabled={!canStart}
            submitting={submitting}
            label={
              draft.scopeKind === "full" ? t("verify.startFull") : t("verify.startPreview")
            }
            blockedReason={startBlockedReason}
            onClick={startRun}
          />
        </aside>
      </div>
    </div>
  );
}

function VerifyRunReview({
  t,
  run,
  state,
  live,
  threshold,
  topN,
  expanded,
  onToggleExpanded,
  onAddFrame,
}: {
  t: Translator;
  run: Run;
  state: ProjectState;
  live: VerifyFrameEntry[] | null;
  threshold: string;
  topN: string;
  expanded: boolean;
  onToggleExpanded: () => void;
  onAddFrame: (entry: VerifyFrameEntry) => void;
}) {
  const stored = useMemo(() => storedVerifyFrames(run), [run]);
  const frames = stored ?? live ?? [];

  const filtered = useMemo(() => {
    const thresholdValue = threshold.trim() ? Number(threshold.trim()) : null;
    const limit = Math.max(1, Number(topN) || 20);
    return reviewVerifyFrames(
      frames,
      thresholdValue !== null && Number.isFinite(thresholdValue) ? thresholdValue : null,
      limit,
    );
  }, [frames, threshold, topN]);

  const coverage = verifyCoverageDisplay(run);
  const concurrency = verifyRunConcurrency(run);

  return (
    <div className="verify-run-block">
      <button
        type="button"
        className="analyze-table-toggle"
        aria-expanded={expanded}
        onClick={onToggleExpanded}
      >
        {expanded ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
        <span className="verify-run-label">{verificationRunLabel(run, state, t)}</span>
        <span className="analyze-table-count">{filtered.length}</span>
        <span className="help-copy">
          {run.status}
          {" · "}
          {t("verify.col.coverage")} {coverage.text}
          {coverage.badge
            ? ` · ${t(coverage.badge === "partial"
                ? "verify.coveragePartial"
                : "verify.coverageIncomplete")}`
            : ""}
          {frames.length ? ` · ${t("verify.frameMetrics", { count: String(frames.length) })}` : ""}
          {concurrency
            ? ` · ${t("verify.concurrencyTelemetry", {
                concurrency: String(concurrency.effective),
                inflight: concurrency.maxInflight == null
                  ? "-"
                  : String(concurrency.maxInflight),
              })}`
            : ""}
        </span>
        {coverage.metricsIncomplete ? (
          <span className="verify-metrics-warning">{t("verify.metricsIncomplete")}</span>
        ) : null}
      </button>

      {expanded ? (
        filtered.length ? (
          <div className="result-table" role="table" aria-label={t("verify.reviewTable")}>
            <div className="result-table-head" role="row">
              <span role="columnheader">{t("verify.col.frame")}</span>
              <span role="columnheader">{t("verify.col.time")}</span>
              <span role="columnheader">{t("verify.col.error")}</span>
              <span role="columnheader" />
            </div>
            {filtered.map((frame) => (
              <div className="result-table-row" role="row" key={frame.seq}>
                <span role="cell">#{frame.frameIndex}</span>
                <span role="cell">
                  {frame.timestampSeconds != null ? `${frame.timestampSeconds.toFixed(3)}s` : "—"}
                </span>
                <span role="cell">{frame.error != null ? frame.error.toPrecision(6) : "—"}</span>
                <span role="cell">
                  <button
                    className="link-button"
                    type="button"
                    onClick={() => onAddFrame(frame)}
                  >
                    {t("verify.addToSamples")}
                  </button>
                </span>
              </div>
            ))}
          </div>
        ) : (
          <p className="help-copy">{frames.length ? t("verify.noFramesAbove") : null}</p>
        )
      ) : null}
    </div>
  );
}

function verifyRunConcurrency(
  run: Run,
): { effective: number; maxInflight: number | null } | null {
  const result = run.result && typeof run.result === "object"
    ? run.result as Record<string, unknown>
    : null;
  const telemetry = result?.telemetry;
  if (telemetry && typeof telemetry === "object" && !Array.isArray(telemetry)) {
    const values = telemetry as Record<string, unknown>;
    const effective = Number(values.effective_concurrency);
    const maxInflight = Number(values.max_inflight);
    if (Number.isInteger(effective) && effective >= 1) {
      return {
        effective,
        maxInflight: Number.isInteger(maxInflight) && maxInflight >= 0
          ? maxInflight
          : null,
      };
    }
  }
  const snapshot = run.inputSnapshot && typeof run.inputSnapshot === "object"
    ? run.inputSnapshot as Record<string, unknown>
    : null;
  const requested = Number(snapshot?.concurrency);
  return Number.isInteger(requested) && requested >= 1
    ? { effective: requested, maxInflight: null }
    : null;
}

function scopeLabel(t: Translator, selection: string): string {
  if (selection === "all") return t("verify.scopeFull");
  if (selection === "decoded_i_picture") return t("verify.ruleIPicture");
  if (selection === "every_n") return t("verify.ruleEveryN");
  return selection;
}
