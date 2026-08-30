import { useEffect, useMemo, useRef, useState } from "react";
import { ChevronDown, ChevronRight, Download, ScanSearch, Trash2 } from "lucide-react";
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
import {
  backendOptionLabel,
  reconcileBackendPreference,
  verifySelectableBackends,
} from "../engine/backendSelection";
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
import { buildVerificationFusion, fusionEligibility } from "../project/verificationFusion";
import { buildVerificationFusionCsv, buildVerificationFusionJson, saveArtifact } from "../project/export";

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
  const [historySourceId, setHistorySourceId] = useState<string>("");
  const [selectedFusionRunIds, setSelectedFusionRunIds] = useState<Set<string>>(new Set());
  const [selectedFusionId, setSelectedFusionId] = useState<string | null>(null);
  const [selectedFusionFrame, setSelectedFusionFrame] = useState<number | null>(null);
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

  const verifyBackends = useMemo(
    () => verifySelectableBackends(capabilities),
    [capabilities],
  );
  const verifyBackendsKey = verifyBackends.join(",");
  useEffect(() => {
    setDraft((current) => {
      const next = reconcileBackendPreference(current.backendPreference, verifyBackends);
      return next === current.backendPreference ? current : { ...current, backendPreference: next };
    });
  }, [verifyBackends, verifyBackendsKey]);

  const plan = useMemo(() => {
    if (!recipe) return null;
    const result = planVerifyRunGroup({
      draft,
      recipe,
      sourcesById: state.sourcesById,
    });
    return result.ok ? result.plan : null;
  }, [draft, recipe, state.sourcesById]);

  const runs = useMemo(
    () => verificationRuns(state).filter((run) => !historySourceId || run.sourceId === historySourceId),
    [state, historySourceId],
  );
  const historySourceIds = useMemo(() => new Set([
    ...verificationRuns(state).map((run) => run.sourceId).filter((id): id is string => Boolean(id)),
    ...Object.values(state.verificationFusionsById).map((fusion) => fusion.sourceId),
  ]), [state]);
  const historySources = useMemo(
    () => {
      const existing = Object.values(state.sourcesById).filter((source) => historySourceIds.has(source.id));
      const known = new Set(existing.map((source) => source.id));
      const virtual = Object.values(state.verificationFusionsById)
        .filter((fusion) => !known.has(fusion.sourceId))
        .map((fusion) => ({
          id: fusion.sourceId,
          kind: "video" as const,
          path: fusion.sourcePath,
          fingerprint: fusion.sourceFingerprint,
          state: "missing" as const,
          label: fusion.sourceLabel,
          videoStreams: [],
          selectedStreamIndex: fusion.streamIndex,
        }));
      return [...existing, ...virtual];
    },
    [state.sourcesById, state.verificationFusionsById, historySourceIds],
  );
  useEffect(() => {
    setHistorySourceId((current) => current && historySources.some((source) => source.id === current)
      ? current
      : historySources[0]?.id ?? "");
  }, [historySources]);
  const historySource = historySourceId ? historySources.find((source) => source.id === historySourceId) ?? null : null;
  const sourceFusions = useMemo(
    () => Object.values(state.verificationFusionsById)
      .filter((fusion) => !historySourceId || fusion.sourceId === historySourceId)
      .sort((a, b) => b.createdAt.localeCompare(a.createdAt)),
    [state.verificationFusionsById, historySourceId],
  );
  const selectedFusion = selectedFusionId ? state.verificationFusionsById[selectedFusionId] ?? null : null;
  const eligibleRunIds = useMemo(() => new Set(
    runs.filter((run) => historySource && fusionEligibility(run, state, historySource).ok).map((run) => run.id),
  ), [runs, state, historySource]);
  const selectedEligibleRunIds = [...selectedFusionRunIds].filter((id) => eligibleRunIds.has(id));
  const fusionBuild = useMemo(() => historySource && selectedEligibleRunIds.length >= 2
    ? buildVerificationFusion({ state, runIds: selectedEligibleRunIds, sourceId: historySource.id })
    : null, [state, selectedEligibleRunIds.join("\u0000"), historySource]);
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
        )
        .concat(selectedFusion ? selectedFusion.frames.map((frame) => ({
          key: `${selectedFusion.id}-${frame.frameIndex}`,
          runId: selectedFusion.id,
          x: String(frame.frameIndex),
          metric: frame.fusedError,
          color: "#111827",
          lineWidth: 3,
          label: `Fusion: ${frame.candidates.map((candidate) => `${selectedFusion.inputs.find((input) => input.recipeId === candidate.recipeId)?.recipeName ?? candidate.recipeId}=${candidate.error}`).join(", ")}`,
        })) : []),
    [runs, hiddenRunIds, liveFrames, runColorById, runLabelById, selectedFusion],
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

  function toggleFusionRun(runId: string) {
    setSelectedFusionRunIds((current) => {
      const next = new Set(current);
      if (next.has(runId)) next.delete(runId); else next.add(runId);
      return next;
    });
  }

  function fusionRunReason(run: Run): string | null {
    if (!historySource) return "source_missing";
    const eligibility = fusionEligibility(run, state, historySource);
    if (!eligibility.ok) return eligibility.reason;
    const anchorId = selectedFusionRunIds.values().next().value as string | undefined;
    if (!anchorId || anchorId === run.id) return null;
    const anchor = state.runsById[anchorId];
    if (!anchor) return "run_missing";
    const pair = buildVerificationFusion({ state, runIds: [anchor.id, run.id], sourceId: historySource.id });
    return pair.ok ? null : pair.reason;
  }

  function createFusion() {
    if (!historySource || selectedEligibleRunIds.length < 2) return;
    const result = buildVerificationFusion({ state, runIds: selectedEligibleRunIds, sourceId: historySource.id });
    if (!result.ok) {
      setNotice(t(`verify.fusionReason.${result.reason}` as never));
      return;
    }
    onProjectChange((current) => ({
      ...current,
      verificationFusionsById: {
        ...current.verificationFusionsById,
        [result.fusion.id]: result.fusion,
      },
    }));
    setSelectedFusionId(result.fusion.id);
    setSelectedFusionFrame(result.fusion.frames[0]?.frameIndex ?? null);
    setNotice(t("verify.fusionCreated"));
  }

  function deleteFusion(fusionId: string) {
    onProjectChange((current) => {
      const next = { ...current.verificationFusionsById };
      delete next[fusionId];
      return { ...current, verificationFusionsById: next };
    });
    if (selectedFusionId === fusionId) setSelectedFusionId(null);
  }

  async function exportFusion(fusion: NonNullable<typeof selectedFusion>, format: "json" | "csv") {
    const path = await saveArtifact({
      defaultName: `${state.project.name || "project"}-${fusion.id}`,
      extension: format,
      content: format === "json" ? buildVerificationFusionJson(fusion) : buildVerificationFusionCsv(fusion),
    });
    setNotice(t("verify.fusionExported", { path }));
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
            <label className="verify-filter verify-history-source">
              <span>{t("verify.historySource")}</span>
              <select
                className="control-select"
                value={historySourceId}
                onChange={(event) => {
                  setHistorySourceId(event.target.value);
                  setSelectedFusionRunIds(new Set());
                  setSelectedFusionId(null);
                }}
              >
                {historySources.map((source) => <option key={source.id} value={source.id}>{source.label || source.path}</option>)}
              </select>
            </label>
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
                <label className="verify-filter">
                  <span>{t("verify.reviewThreshold")}</span>
                  <input
                    value={reviewThreshold}
                    placeholder={t("verify.thresholdOff")}
                    onChange={(event) => setReviewThreshold(event.target.value)}
                  />
                </label>
                <label className="verify-filter">
                  <span>{t("verify.topN")}</span>
                  <input
                    value={topN}
                    onChange={(event) => setTopN(event.target.value)}
                  />
                </label>
              </div>
            ) : null}
          </div>
          {runs.length > 0 || sourceFusions.length > 0 ? (
            <div className="verify-fusion-picker">
              <div className="verify-fusion-picker-head">
                <h3>{t("verify.fusionTitle")}</h3>
                <button
                  className="secondary-button"
                  type="button"
                  disabled={selectedEligibleRunIds.length < 2 || !fusionBuild?.ok}
                  onClick={createFusion}
                >
                  {t("verify.fusionCreate")}
                </button>
              </div>
              <div className="verify-fusion-run-list">
                {runs.map((run) => {
                  const reason = fusionRunReason(run);
                  const eligible = reason === null;
                  const checked = selectedFusionRunIds.has(run.id);
                  const reasonKey = `verify.fusionReason.${reason ?? ""}`;
                  return (
                    <label key={run.id} className={`verify-fusion-run${!eligible ? " is-disabled" : ""}`} title={eligible ? undefined : t(reasonKey as never)}>
                      <input type="checkbox" checked={checked} disabled={!eligible} onChange={() => toggleFusionRun(run.id)} />
                      <span>{runLabelById.get(run.id)}</span>
                      <small>{eligible ? run.status : t(reasonKey as never)}</small>
                    </label>
                  );
                })}
              </div>
              {sourceFusions.length > 0 ? (
                <div className="verify-saved-fusions">
                  <label className="verify-filter">
                    <span>{t("verify.savedFusions")}</span>
                    <select className="control-select" value={selectedFusionId ?? ""} onChange={(event) => {
                      const nextId = event.target.value || null;
                      setSelectedFusionId(nextId);
                      const nextFusion = nextId ? state.verificationFusionsById[nextId] : null;
                      setSelectedFusionFrame(nextFusion?.frames[0]?.frameIndex ?? null);
                    }}>
                      <option value="">{t("verify.noFusionSelected")}</option>
                      {sourceFusions.map((fusion) => <option key={fusion.id} value={fusion.id}>{fusion.id} · {fusion.inputs.map((input) => input.recipeName).join(" + ")}</option>)}
                    </select>
                  </label>
                  {selectedFusion ? (
                    <div className="verify-fusion-actions">
                      <button className="secondary-button" type="button" onClick={() => void exportFusion(selectedFusion, "json")}><Download size={14} />{t("verify.exportJson")}</button>
                      <button className="secondary-button" type="button" onClick={() => void exportFusion(selectedFusion, "csv")}><Download size={14} />{t("verify.exportCsv")}</button>
                      <button className="link-button" type="button" onClick={() => deleteFusion(selectedFusion.id)}><Trash2 size={14} />{t("verify.deleteFusion")}</button>
                    </div>
                  ) : null}
                </div>
              ) : null}
            </div>
          ) : null}
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
                  selectedX={selectedFusionFrame == null ? null : String(selectedFusionFrame)}
                  onSelect={(x) => selectedFusion && setSelectedFusionFrame(Number(x))}
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
          {selectedFusion ? (
            <div className="verify-fusion-summary">
              <strong>{t("verify.fusionSummary")}</strong>
              <span>{selectedFusion.inputs.map((input) => input.recipeName).join(" + ")}</span>
              <span>{t("verify.fusionCoverage", { count: String(selectedFusion.statistics.totalFrames) })}</span>
              <span>{t("verify.fusionSingle", { count: String(selectedFusion.statistics.singleCandidateFrames) })}</span>
              <span>{t("verify.fusionMulti", { count: String(selectedFusion.statistics.multiCandidateFrames) })}</span>
              <span>{t("verify.fusionTies", { count: String(selectedFusion.statistics.tiedFrames) })}</span>
              <div className="verify-fusion-wins">
                {selectedFusion.inputs.map((input) => <span key={input.recipeId}>{input.recipeName}: {selectedFusion.statistics.winsByRecipe[input.recipeId] ?? 0}</span>)}
              </div>
            </div>
          ) : null}
          {selectedFusion && selectedFusionFrame != null ? (() => {
            const frame = selectedFusion.frames.find((entry) => entry.frameIndex === selectedFusionFrame);
            return frame ? (
              <div className="verify-fusion-frame-detail">
                <label className="verify-filter"><span>{t("verify.fusionFrame")}</span><select className="control-select" value={String(selectedFusionFrame)} onChange={(event) => setSelectedFusionFrame(Number(event.target.value))}>{selectedFusion.frames.map((entry) => <option key={entry.frameIndex} value={entry.frameIndex}>#{entry.frameIndex}</option>)}</select></label>
                <span>{t("verify.fusionWinner", { recipe: selectedFusion.inputs.find((input) => input.recipeId === frame.winnerRecipeId)?.recipeName ?? frame.winnerRecipeId, error: frame.fusedError.toPrecision(6) })}</span>
                <span>{frame.candidates.map((candidate) => `${selectedFusion.inputs.find((input) => input.recipeId === candidate.recipeId)?.recipeName ?? candidate.recipeId}: ${candidate.error.toPrecision(6)}`).join(" · ")}</span>
              </div>
            ) : null;
          })() : null}
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
                capabilities?.payload.features?.verify_engine_decode === true,
              )}
              onChange={(event) =>
                patch({ backendPreference: event.target.value as BackendPreference })
              }
            >
              {verifyBackends.map((backend) => (
                <option key={backend} value={backend}>
                  {backendOptionLabel(
                    t,
                    backend,
                    capabilities,
                    recipe?.metric?.pNorm ?? 1,
                    undefined,
                    capabilities?.payload.features?.verify_engine_decode !== true,
                    capabilities?.payload.features?.verify_engine_decode === true,
                  )}
                </option>
              ))}
            </select>
          </label>

          <label className="block">
            <span>{t("verify.concurrency")}</span>
            <input
              inputMode="numeric"
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
