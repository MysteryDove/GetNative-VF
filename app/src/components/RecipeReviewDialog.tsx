import { Lock, X } from "lucide-react";
import type { Translator } from "../i18n";
import { kernelDisplayName, profileDisplayName } from "../engine/displayNames";
import { recipeLockReadiness } from "../project/recipe";
import type { Recipe } from "../project/types";

const MISSING_LABEL_KEYS = {
  name: "recipe.missing.name",
  geometry: "recipe.missing.geometry",
  kernel: "recipe.missing.kernel",
  metric: "recipe.missing.metric",
  metric_invalid: "recipe.missing.metricInvalid",
  profile: "recipe.missing.profile",
  math_mode: "recipe.missing.mathMode",
} as const;

/**
 * Recipe Review: the full semantic summary is inspected before locking.
 * Locking makes the Recipe immutable; activation stays a separate explicit
 * action. Which MetricSpec the Recipe owns is decided upstream by explicit
 * apply actions (Resolution Test by default, or the diverged Algorithm Test
 * MetricSpec via its own explicit command) — this dialog shows the result.
 */
export function RecipeReviewDialog({
  t,
  recipe,
  onClose,
  onRename,
  onLock,
  onActivate,
}: {
  t: Translator;
  recipe: Recipe;
  onClose: () => void;
  onRename: (name: string) => void;
  onLock: () => void;
  onActivate: () => void;
}) {
  const readiness = recipeLockReadiness(recipe);
  const canLock = recipe.status === "draft" && readiness.ok;

  return (
    <div className="modal-backdrop" role="presentation" onClick={onClose}>
      <div
        className="modal-card"
        role="dialog"
        aria-modal="true"
        aria-label={t("recipe.reviewTitle")}
        onClick={(event) => event.stopPropagation()}
      >
        <div className="modal-header">
          <h3>{t("recipe.reviewTitle")}</h3>
          <button className="icon-button" type="button" onClick={onClose} aria-label={t("common.close")}>
            <X size={16} />
          </button>
        </div>

        {recipe.status === "draft" ? (
          <label className="block">
            <span>{t("recipe.name")}</span>
            <input
              value={recipe.name}
              onChange={(event) => onRename(event.target.value)}
            />
          </label>
        ) : (
          <p>
            <strong>{recipe.name}</strong>
          </p>
        )}

        <div className="dense-table recipe-summary">
          <div className="dense-row">
            <strong>{t("recipe.field.geometry")}</strong>
            <span>
              {recipe.geometry
                ? `${recipe.geometry.canvasWidth}×${recipe.geometry.canvasHeight}` +
                  ` · src (${recipe.geometry.srcLeft}, ${recipe.geometry.srcTop}) ` +
                  `${recipe.geometry.srcWidth}×${recipe.geometry.srcHeight}`
                : t("recipe.fieldMissing")}
            </span>
          </div>
          <div className="dense-row">
            <strong>{t("recipe.field.kernel")}</strong>
            <span>
              {recipe.kernel?.id
                ? kernelDisplayName(t, recipe.kernel.id) +
                  (Object.keys(recipe.kernel.parameters).length
                    ? ` (${Object.entries(recipe.kernel.parameters)
                        .map(([key, value]) => `${key}=${value}`)
                        .join(", ")})`
                    : "")
                : t("recipe.fieldMissing")}
            </span>
          </div>
          <div className="dense-row">
            <strong>{t("recipe.field.metric")}</strong>
            <span>
              {recipe.metric
                ? `crop ${recipe.metric.cropLeft}/${recipe.metric.cropRight}/` +
                  `${recipe.metric.cropTop}/${recipe.metric.cropBottom}` +
                  ` · ${t("analyze.pixelExclusion")} ${recipe.metric.pixelExclusionThreshold}` +
                  ` · p=${recipe.metric.pNorm}`
                : t("recipe.fieldMissing")}
            </span>
          </div>
          <div className="dense-row">
            <strong>{t("recipe.field.profile")}</strong>
            <span>
              {recipe.profileId ? profileDisplayName(t, recipe.profileId) : t("recipe.fieldMissing")}
            </span>
          </div>
          <div className="dense-row">
            <strong>{t("recipe.field.mathMode")}</strong>
            <span>{recipe.mathMode ?? t("recipe.fieldMissing")}</span>
          </div>
        </div>

        {!readiness.ok ? (
          <div className="review-blockers">
            <p className="help-copy warning-copy">{t("recipe.lockIncomplete")}</p>
            <ul className="blocker-list">
              {readiness.missing.map((field) => (
                <li key={field}>
                  {t(MISSING_LABEL_KEYS[field as keyof typeof MISSING_LABEL_KEYS] ?? "recipe.fieldMissing")}
                </li>
              ))}
            </ul>
          </div>
        ) : null}

        <div className="modal-actions">
          {recipe.status === "draft" ? (
            <button
              className="primary-button"
              type="button"
              disabled={!canLock}
              onClick={onLock}
            >
              <Lock size={15} />
              {t("recipe.lock")}
            </button>
          ) : null}
          {recipe.status === "locked" ? (
            <button className="primary-button" type="button" onClick={onActivate}>
              {t("recipe.activate")}
            </button>
          ) : null}
          <button className="secondary-button" type="button" onClick={onClose}>
            {t("common.close")}
          </button>
        </div>
        {recipe.status === "draft" ? (
          <p className="help-copy">{t("recipe.lockHint")}</p>
        ) : null}
      </div>
    </div>
  );
}
