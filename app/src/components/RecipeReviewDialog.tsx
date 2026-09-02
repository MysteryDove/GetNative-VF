import type { Translator } from "../i18n";
import { kernelDisplayName } from "../engine/displayNames";
import { recipeReadiness } from "../project/recipe";
import type { Recipe } from "../project/types";
import { Modal } from "./Modal";

export const MISSING_LABEL_KEYS = {
  name: "recipe.missing.name",
  geometry: "recipe.missing.geometry",
  geometry_review: "recipe.missing.geometryReview",
  kernel: "recipe.missing.kernel",
  metric: "recipe.missing.metric",
  metric_invalid: "recipe.missing.metricInvalid",
  profile: "recipe.missing.profile",
  math_mode: "recipe.missing.mathMode",
} as const;

/** Localize a `recipeReadiness` missing-code list for notices/dialogs. */
export function missingFieldLabels(t: Translator, missing: string[]): string {
  return missing
    .map((field) =>
      t(MISSING_LABEL_KEYS[field as keyof typeof MISSING_LABEL_KEYS] ?? "recipe.fieldMissing"),
    )
    .join(" · ");
}

/**
 * Recipe detail: the full semantic summary of one Recipe, including any
 * missing fields that would block whole-video Verification. Which MetricSpec
 * the Recipe owns is decided upstream by explicit apply actions (Resolution
 * Test by default, or the diverged Algorithm Test MetricSpec via its own
 * explicit command) — this dialog shows the result.
 */
export function RecipeReviewDialog({
  t,
  recipe,
  isActive,
  onClose,
  onSetActive,
}: {
  t: Translator;
  recipe: Recipe;
  isActive: boolean;
  onClose: () => void;
  onSetActive: () => void;
}) {
  const readiness = recipeReadiness(recipe);

  return (
    <Modal
      onClose={onClose}
      title={t("recipe.reviewTitle")}
      closeLabel={t("common.close")}
      actions={
        <>
          {!isActive ? (
            <button className="primary-button" type="button" onClick={onSetActive}>
              {t("recipe.setActive")}
            </button>
          ) : null}
          <button className="secondary-button" type="button" onClick={onClose}>
            {t("common.close")}
          </button>
        </>
      }
    >
      <p>
        <strong>{recipe.name}</strong>
      </p>

      <div className="dense-table recipe-summary">
        <div className="dense-row">
          <strong>{t("recipe.field.geometry")}</strong>
          <span>
            {recipe.geometry
              ? `${recipe.geometry.canvasWidth}×${recipe.geometry.canvasHeight}` +
                ` · src (${recipe.geometry.srcLeft}, ${recipe.geometry.srcTop}) ` +
                `${recipe.geometry.srcWidth}×${recipe.geometry.srcHeight}` +
                ` · base ${recipe.geometry.baseWidth ?? "integer"}×${recipe.geometry.baseHeight ?? "integer"}`
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
    </Modal>
  );
}
