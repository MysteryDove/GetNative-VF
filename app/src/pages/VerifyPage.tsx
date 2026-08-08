import { useEffect, useMemo, useRef, useState } from "react";
import { Play } from "lucide-react";
import type { Translator } from "../i18n";
import type { EngineEnvelope } from "../engine/types";
import { kernelDisplayName, profileDisplayName } from "../engine/displayNames";
import type { BackendPreference } from "../engine/protocol";
import { activeRecipe } from "../project/recipe";
import { selectableBackends } from "../engine/heightDraft";
import {
  defaultVerifyDraft,
  planVerifyRunGroup,
  verificationRuns,
  type VerifyDraft,
} from "../engine/verifyPlan";
import { startVerifyRunGroup, type VerifyFrameEntry } from "../engine/executeVerify";
import type { ExecutionBridge } from "../engine/executeRunGroup";
import type { ProjectRoute, ProjectState, Run, Sample } from "../project/types";

/** Stored verify result shape: engine payload + orchestrator-merged frames. */
export function storedVerifyFrames(run: Run): VerifyFrameEntry[] | null {
  if (!run.result || typeof run.result !== "object") return null;
  const frames = (run.result as Record<string, unknown>).frames;
  if (!Array.isArray(frames)) return null;
  const rows: VerifyFrameEntry[] = [];
  for (const item of frames) {
    if (!item || typeof item !== "object") continue;
    const row = item as Record<string, unknown>;
    if (typeof row.frameIndex !== "number") continue;
    rows.push({
      seq: typeof row.seq === "number" ? row.seq : row.frameIndex,
      frameIndex: row.frameIndex,
      pts: typeof row.pts === "number" ? row.pts : null,
      timestampSeconds: typeof row.timestampSeconds === "number" ? row.timestampSeconds : null,
      error: typeof row.error === "number" ? row.error : null,
    });
  }
  return rows.length ? rows : null;
}

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
  const [submitting, setSubmitting] = useState(false);
  const [notice, setNotice] = useState("");
  const [liveFrames, setLiveFrames] = useState<Record<string, VerifyFrameEntry[]>>({});
  const [reviewThreshold, setReviewThreshold] = useState("");
  const [topN, setTopN] = useState("20");
  const mounted = useRef(true);
  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
    };
  }, []);

  const recipe = activeRecipe(state);
  const readyVideos = useMemo(
    () =>
      Object.values(state.sourcesById).filter(
        (source) => source.kind === "video" && source.state === "ready",
      ),
    [state.sourcesById],
  );

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

  const startBlockedReason = !analyzeAvailable
    ? t("verify.blocked.noCommand")
    : !recipe
      ? t("verify.blocked.noRecipe")
      : draft.sourceIds.length === 0
        ? t("verify.blocked.noSources")
        : !plan
          ? t("verify.blocked.invalidPlan")
          : null;

  const canStart = analyzeAvailable && plan !== null && !submitting;

  async function startRun() {
    if (!plan || !recipe || submitting) return;
    setSubmitting(true);
    setNotice("");
    try {
      const result = await startVerifyRunGroup({
        plan,
        recipe,
        state,
        onProjectChange,
        bridge: executionBridge,
        onFrames: (runId, entries) => {
          if (!mounted.current) return;
          setLiveFrames((current) => ({
            ...current,
            [runId]: [...(current[runId] ?? []), ...entries],
          }));
        },
      });
      if (!result.ok) {
        setNotice(t("analyze.submitFailed", { detail: result.reason }));
        return;
      }
      setNotice(
        t("analyze.runSubmitted", {
          submitted: String(result.submitted),
          failedNote: result.failed > 0 ? `, ${result.failed} failed` : "",
        }),
      );
    } catch (error) {
      setNotice(t("analyze.submitFailed", { detail: String(error) }));
    } finally {
      setSubmitting(false);
    }
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
    const order =
      Math.max(-1, ...Object.values(state.samplesById).map((sample) => sample.order)) + 1;
    const stream = source.videoStreams.find((item) => item.index === streamIndex);
    const sample: Sample = {
      id: `smp_${crypto.randomUUID()}`,
      sourceId: source.id,
      sourceFingerprint: source.fingerprint ?? null,
      label: `${source.label || source.path} #${entry.frameIndex}`,
      included: true,
      order,
      frameIndex: entry.frameIndex,
      streamIndex,
      pts: entry.pts ?? null,
      bestEffortTimestamp: entry.pts ?? null,
      timeBaseNum: stream?.timeBaseNum ?? null,
      timeBaseDen: stream?.timeBaseDen ?? null,
      timestampSeconds: entry.timestampSeconds ?? null,
      tags: [],
      originRunId: run.id,
    };
    onProjectChange((current) => ({
      ...current,
      samplesById: { ...current.samplesById, [sample.id]: sample },
    }));
    setNotice(t("verify.sampleAdded", { frame: String(entry.frameIndex) }));
  }

  return (
    <div className="page-panel">
      <div className="page-header">
        <h2>{t("verify.title")}</h2>
      </div>

      <section className="page-section verify-recipe-strip">
        <h3>{t("verify.activeRecipe")}</h3>
        {recipe ? (
          <div className="dense-table">
            <div className="dense-row">
              <strong>
                {recipe.name}
                <span className="recipe-badge active">{t("recipe.active")}</span>
              </strong>
              <span>
                {t("recipe.revision", { revision: recipe.revision })}
                {recipe.geometry
                  ? ` · ${recipe.geometry.canvasWidth}×${recipe.geometry.canvasHeight}`
                  : ""}
                {recipe.kernel?.id ? ` · ${kernelDisplayName(t, recipe.kernel.id)}` : ""}
                {recipe.profileId ? ` · ${profileDisplayName(t, recipe.profileId)}` : ""}
                {recipe.mathMode ? ` · ${recipe.mathMode}` : ""}
                {recipe.metric
                  ? ` · ${t("analyze.pixelExclusion")} ${recipe.metric.pixelExclusionThreshold}`
                  : ""}
              </span>
            </div>
            <p className="help-copy">{t("verify.recipeReadOnly")}</p>
          </div>
        ) : (
          <div className="empty-inline">
            <p>{t("verify.noActiveRecipe")}</p>
            <button className="secondary-button" type="button" onClick={() => onNavigate("overview")}>
              {t("verify.changeRecipe")}
            </button>
          </div>
        )}
        {recipe ? (
          <button className="link-button" type="button" onClick={() => onNavigate("overview")}>
            {t("verify.changeRecipe")}
          </button>
        ) : null}
      </section>

      <div className="analyze-layout">
        <aside className="analyze-samples pane">
          <h3>{t("verify.sourcesTitle")}</h3>
          {readyVideos.length === 0 ? (
            <div className="empty-inline">
              <p>{t("verify.noVideos")}</p>
              <button className="secondary-button" type="button" onClick={() => onNavigate("media")}>
                {t("nav.media")}
              </button>
            </div>
          ) : (
            <ul className="analyze-sample-list">
              {readyVideos.map((source) => (
                <li key={source.id}>
                  <label className="checkbox-row">
                    <input
                      type="checkbox"
                      checked={draft.sourceIds.includes(source.id)}
                      onChange={() => toggleSource(source.id)}
                    />
                    <span>
                      {source.label || source.path}
                      {source.width && source.height
                        ? ` · ${source.width}×${source.height}`
                        : ""}
                      {source.durationSeconds != null
                        ? ` · ${source.durationSeconds.toFixed(1)}s`
                        : ""}
                    </span>
                  </label>
                </li>
              ))}
            </ul>
          )}

          {plan ? (
            <div className="run-group-plan">
              <h3>{t("analyze.runGroupPlan")}</h3>
              <p className="help-copy">
                {t("analyze.runGroupType", { type: plan.groupType })}
                {" · "}
                {t("analyze.memberCount", { count: String(plan.memberCount) })}
              </p>
              <ul className="run-group-members">
                {plan.members.slice(0, 12).map((member) => (
                  <li key={member.planKey}>
                    <strong>{member.sourceLabel}</strong>
                    <span>{scopeLabel(t, member.scanScope.selection)}</span>
                  </li>
                ))}
              </ul>
              {plan.memberCount > 1 ? (
                <p className="help-copy">{t("verify.memberPerSource")}</p>
              ) : null}
            </div>
          ) : null}
        </aside>

        <section className="analyze-plot pane">
          <div className="analyze-table-toolbar">
            <h3>{t("verify.reviewTitle")}</h3>
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
          <p className="help-copy">{t("verify.reviewFilterHint")}</p>
          {notice ? <p className="help-copy">{notice}</p> : null}
          {runs.length === 0 ? (
            <p className="empty-copy">{t("verify.noRuns")}</p>
          ) : (
            runs.map((run) => (
              <VerifyRunReview
                key={run.id}
                t={t}
                run={run}
                state={state}
                live={liveFrames[run.id] ?? null}
                threshold={reviewThreshold}
                topN={topN}
                onAddFrame={(entry) => addFrameToSamples(run, entry)}
              />
            ))
          )}
        </section>

        <aside className="analyze-params pane">
          <h3>{t("verify.setupTitle")}</h3>

          <fieldset className={`metric-fieldset scope-${draft.scopeKind}`}>
            <legend>{t("verify.scope")}</legend>
            <label className={`checkbox-row scope-option scope-full ${draft.scopeKind === "full" ? "active" : ""}`}>
              <input
                type="radio"
                name="verify-scope"
                checked={draft.scopeKind === "full"}
                onChange={() => patch({ scopeKind: "full" })}
              />
              <span>{t("verify.scopeFull")}</span>
            </label>
            <p className="help-copy">{t("verify.scopeFullHint")}</p>
            <label className={`checkbox-row scope-option scope-preview ${draft.scopeKind === "preview" ? "active" : ""}`}>
              <input
                type="radio"
                name="verify-scope"
                checked={draft.scopeKind === "preview"}
                onChange={() => patch({ scopeKind: "preview" })}
              />
              <span>{t("verify.scopePreview")}</span>
            </label>
            <p className="help-copy">{t("verify.scopePreviewHint")}</p>
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
              value={draft.backendPreference}
              onChange={(event) =>
                patch({ backendPreference: event.target.value as BackendPreference })
              }
            >
              {selectableBackends(capabilities).map((backend) => (
                <option key={backend} value={backend}>
                  {backend === "auto"
                    ? t("analyze.backend.auto")
                    : t(`backend.${backend}` as "backend.cpu")}
                </option>
              ))}
            </select>
          </label>

          <div className="analyze-run-block">
            <button
              className="primary-button"
              type="button"
              disabled={!canStart}
              onClick={startRun}
            >
              <Play size={15} />
              {submitting
                ? t("diagnostics.working")
                : draft.scopeKind === "full"
                  ? t("verify.startFull")
                  : t("verify.startPreview")}
            </button>
            {startBlockedReason ? <p className="help-copy">{startBlockedReason}</p> : null}
          </div>
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
  onAddFrame,
}: {
  t: Translator;
  run: Run;
  state: ProjectState;
  live: VerifyFrameEntry[] | null;
  threshold: string;
  topN: string;
  onAddFrame: (entry: VerifyFrameEntry) => void;
}) {
  const stored = useMemo(() => storedVerifyFrames(run), [run]);
  const frames = stored ?? live ?? [];
  const source = run.sourceId ? state.sourcesById[run.sourceId] : null;

  const filtered = useMemo(() => {
    const thresholdValue = threshold.trim() ? Number(threshold.trim()) : null;
    const limit = Math.max(1, Number(topN) || 20);
    const valid = frames.filter((frame) => frame.error !== null);
    const above =
      thresholdValue !== null && Number.isFinite(thresholdValue)
        ? valid.filter((frame) => (frame.error as number) > thresholdValue)
        : valid;
    return [...above].sort((a, b) => (b.error as number) - (a.error as number)).slice(0, limit);
  }, [frames, threshold, topN]);

  const timeline = useMemo(() => bucketTimeline(frames, 240), [frames]);
  const coverage =
    run.total > 0 ? `${Math.min(run.completed, run.total)}/${run.total}` : `${run.completed}`;

  return (
    <div className="verify-run-block">
      <div className="dense-row">
        <strong>{source?.label || source?.path || run.sourceId || "—"}</strong>
        <span>
          {run.status}
          {" · "}
          {t("verify.col.coverage")} {coverage}
          {frames.length ? ` · ${t("verify.frameMetrics", { count: String(frames.length) })}` : ""}
        </span>
      </div>

      {timeline.length ? (
        <div className="verify-timeline" aria-label={t("verify.timeline")}>
          {timeline.map((bucket, index) => (
            <div
              key={index}
              className="verify-timeline-bar"
              style={{ height: `${Math.max(3, bucket.normalized * 100)}%` }}
              title={`#${bucket.frameIndex}: ${bucket.error?.toPrecision(3) ?? "—"}`}
            />
          ))}
        </div>
      ) : null}

      {filtered.length ? (
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
      )}
    </div>
  );
}

/** Bucket many frames into a bounded timeline; each bucket keeps its max-error frame. */
function bucketTimeline(
  frames: VerifyFrameEntry[],
  maxBuckets: number,
): Array<{ frameIndex: number; error: number | null; normalized: number }> {
  const valid = frames.filter((frame) => frame.error !== null);
  if (!valid.length) return [];
  const sorted = [...valid].sort((a, b) => a.frameIndex - b.frameIndex);
  const buckets: Array<{ frameIndex: number; error: number | null }> = [];
  if (sorted.length <= maxBuckets) {
    buckets.push(...sorted.map((frame) => ({ frameIndex: frame.frameIndex, error: frame.error })));
  } else {
    const perBucket = Math.ceil(sorted.length / maxBuckets);
    for (let i = 0; i < sorted.length; i += perBucket) {
      const slice = sorted.slice(i, i + perBucket);
      const worst = slice.reduce((a, b) => ((b.error as number) > (a.error as number) ? b : a));
      buckets.push({ frameIndex: worst.frameIndex, error: worst.error });
    }
  }
  const maxLog = Math.max(
    1e-12,
    ...buckets.map((bucket) => Math.abs(Math.log10((bucket.error as number) + 1e-9))),
  );
  return buckets.map((bucket) => ({
    ...bucket,
    normalized: Math.abs(Math.log10((bucket.error as number) + 1e-9)) / maxLog,
  }));
}

function scopeLabel(t: Translator, selection: string): string {
  if (selection === "all") return t("verify.scopeFull");
  if (selection === "decoded_i_picture") return t("verify.ruleIPicture");
  if (selection === "every_n") return t("verify.ruleEveryN");
  return selection;
}
