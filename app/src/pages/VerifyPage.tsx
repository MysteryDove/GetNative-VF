import { useMemo, useState } from "react";
import { Play } from "lucide-react";
import type { Translator } from "../i18n";
import type { EngineEnvelope } from "../engine/types";
import { kernelDisplayName, profileDisplayName } from "../engine/displayNames";
import type { BackendPreference } from "../engine/protocol";
import { activeRecipe } from "../project/recipe";
import {
  defaultVerifyDraft,
  extractVerifyFrames,
  planVerifyRunGroup,
  verificationRuns,
  type VerifyDraft,
} from "../engine/verifyPlan";
import type { ProjectRoute, ProjectState } from "../project/types";

/**
 * Whole-video Verification setup. Full and Preview scopes are visually
 * distinct; the active locked Recipe is read-only here; Start stays
 * capability-gated until the engine provides a real verify command.
 */
export function VerifyPage({
  t,
  state,
  capabilities,
  analyzeAvailable,
  onNavigate,
}: {
  t: Translator;
  state: ProjectState;
  capabilities: EngineEnvelope | null;
  analyzeAvailable: boolean;
  onNavigate: (route: ProjectRoute) => void;
}) {
  void capabilities;
  const [draft, setDraft] = useState<VerifyDraft>(() => defaultVerifyDraft());

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
          : t("analyze.runBlocked.noWorker");

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
          <h3>{t("verify.reviewTitle")}</h3>
          {runs.length === 0 ? (
            <p className="empty-copy">{t("verify.noRuns")}</p>
          ) : (
            <div className="result-table" role="table" aria-label={t("verify.reviewTitle")}>
              <div className="result-table-head" role="row">
                <span role="columnheader">{t("verify.col.source")}</span>
                <span role="columnheader">{t("verify.col.status")}</span>
                <span role="columnheader">{t("verify.col.coverage")}</span>
                <span role="columnheader">{t("verify.col.frames")}</span>
              </div>
              {runs.map((run) => {
                const frames = extractVerifyFrames(run.result);
                const source = run.sourceId ? state.sourcesById[run.sourceId] : null;
                return (
                  <div className="result-table-row" role="row" key={run.id}>
                    <span role="cell">{source?.label || source?.path || run.sourceId || "—"}</span>
                    <span role="cell">{run.status}</span>
                    <span role="cell">
                      {run.completed}/{run.total}
                    </span>
                    <span role="cell">
                      {frames ? t("verify.frameMetrics", { count: String(frames.length) }) : "—"}
                    </span>
                  </div>
                );
              })}
            </div>
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
              <option value="auto">{t("analyze.backend.auto")}</option>
              <option value="cpu">{t("backend.cpu")}</option>
              <option value="metal">{t("backend.metal")}</option>
            </select>
          </label>

          <div className="analyze-run-block">
            <button className="primary-button" type="button" disabled>
              <Play size={15} />
              {draft.scopeKind === "full" ? t("verify.startFull") : t("verify.startPreview")}
            </button>
            <p className="help-copy">{startBlockedReason}</p>
          </div>
        </aside>
      </div>
    </div>
  );
}

function scopeLabel(t: Translator, selection: string): string {
  if (selection === "all") return t("verify.scopeFull");
  if (selection === "decoded_i_picture") return t("verify.ruleIPicture");
  if (selection === "every_n") return t("verify.ruleEveryN");
  return selection;
}
