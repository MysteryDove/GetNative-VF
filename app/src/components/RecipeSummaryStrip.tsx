import type { Translator } from "../i18n";
import { kernelDisplayName } from "../engine/displayNames";
import type { Recipe } from "../project/types";

/**
 * One-line Recipe identity + semantic summary (revision, canvas, kernel,
 * math mode, pixel exclusion). Shared by the Verify page strip and
 * the Analyze page current-recipe strip so the rendering never drifts.
 */
export function RecipeSummaryStrip({
  t,
  recipe,
  activeRecipeId,
  emptyLabel,
}: {
  t: Translator;
  recipe: Recipe | null;
  activeRecipeId?: string | null;
  emptyLabel: string;
}) {
  if (!recipe) {
    return (
      <div className="dense-table">
        <div className="dense-row">
          <span className="help-copy">{emptyLabel}</span>
        </div>
      </div>
    );
  }
  const isActive = activeRecipeId != null && recipe.id === activeRecipeId;
  return (
    <div className="dense-table">
      <div className="dense-row">
        <strong>
          {recipe.name}
          {isActive ? <span className="recipe-badge active">{t("recipe.active")}</span> : null}
        </strong>
        <span>
          {t("recipe.revision", { revision: recipe.revision })}
          {recipe.geometry
            ? ` · ${recipe.geometry.canvasWidth}×${recipe.geometry.canvasHeight}` +
              ` · src ${recipe.geometry.srcWidth}×${recipe.geometry.srcHeight}` +
              ` · base ${recipe.geometry.baseWidth ?? "integer"}×${recipe.geometry.baseHeight ?? "integer"}` +
              (recipe.geometry.needsReview ? ` · ${t("recipe.missing.geometryReview")}` : "")
            : ""}
          {recipe.kernel?.id ? ` · ${kernelDisplayName(t, recipe.kernel.id)}` : ""}
          {recipe.mathMode ? ` · ${recipe.mathMode}` : ""}
          {recipe.metric
            ? ` · ${t("analyze.pixelExclusion")} ${recipe.metric.pixelExclusionThreshold}`
            : ""}
        </span>
      </div>
    </div>
  );
}
