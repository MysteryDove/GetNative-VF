import { useState } from "react";
import type { Translator } from "../i18n";
import {
  activateRecipeInState,
  createRecipe,
  recipeReadiness,
  recipesByUpdatedAt,
  removeRecipeInState,
} from "../project/recipe";
import type { ProjectState } from "../project/types";
import { missingFieldLabels } from "./RecipeReviewDialog";
import { RecipeReviewDialog } from "./RecipeReviewDialog";

/**
 * Recipe list on the Project Overview: every Recipe is mutable; the current
 * one (active_recipe_id) is the analysis apply target and Verification input.
 * Rows expose set-current, inspect, and guarded removal; incomplete Recipes
 * show their missing fields inline.
 */
export function RecipeManager({
  t,
  state,
  onProjectChange,
}: {
  t: Translator;
  state: ProjectState;
  onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
}) {
  const [reviewingId, setReviewingId] = useState<string | null>(null);
  const [actionError, setActionError] = useState("");

  const recipes = recipesByUpdatedAt(state);
  const reviewing = reviewingId ? (state.recipesById[reviewingId] ?? null) : null;

  function run(op: (current: ProjectState) => { ok: boolean; state?: ProjectState; reason?: string }) {
    // Evaluate against the rendered state synchronously; ops are pure
    // transforms, so the updater only needs to install the computed result.
    const result = op(state);
    if (!result.ok || !result.state) {
      setActionError(t(`recipe.error.${result.reason ?? "unknown"}` as Parameters<Translator>[0]));
      return;
    }
    setActionError("");
    const next = result.state;
    onProjectChange(() => next);
  }

  return (
    <div className="recipe-manager">
      <div className="recipe-manager-actions">
        <button
          className="secondary-button"
          type="button"
          onClick={() =>
            run((current) =>
              createRecipe(current, {
                name: `${t("recipe.defaultName")} ${Object.keys(current.recipesById).length + 1}`,
              }),
            )
          }
        >
          {t("recipe.newDraft")}
        </button>
      </div>
      {recipes.length === 0 ? (
        <p className="empty-copy">{t("overview.recipesEmpty")}</p>
      ) : (
        <div className="dense-table">
          {recipes.map((recipe) => {
            const isActive = state.project.activeRecipeId === recipe.id;
            const readiness = recipeReadiness(recipe);
            return (
              <div className="dense-row recipe-row" key={recipe.id}>
                <strong>
                  {recipe.name}
                  {isActive ? (
                    <span className="recipe-badge active">{t("recipe.active")}</span>
                  ) : null}
                </strong>
                <span>
                  {t("recipe.revision", { revision: recipe.revision })}
                  {recipe.updatedAt ? ` · ${recipe.updatedAt.slice(0, 10)}` : ""}
                  {!readiness.ok
                    ? ` · ${t("recipe.missingFields", { missing: missingFieldLabels(t, readiness.missing) })}`
                    : ""}
                </span>
                <span className="recipe-actions">
                  {!isActive ? (
                    <button
                      className="link-button"
                      type="button"
                      onClick={() =>
                        run((current) => activateRecipeInState(current, recipe.id))
                      }
                    >
                      {t("recipe.setActive")}
                    </button>
                  ) : null}
                  <button
                    className="link-button"
                    type="button"
                    onClick={() => setReviewingId(recipe.id)}
                  >
                    {t("recipe.inspect")}
                  </button>
                  {!isActive ? (
                    <button
                      className="link-button danger"
                      type="button"
                      onClick={() =>
                        run((current) => removeRecipeInState(current, recipe.id))
                      }
                    >
                      {t("recipe.remove")}
                    </button>
                  ) : null}
                </span>
              </div>
            );
          })}
        </div>
      )}
      {actionError ? <p className="help-copy warning-copy">{actionError}</p> : null}

      {reviewing ? (
        <RecipeReviewDialog
          t={t}
          recipe={reviewing}
          isActive={state.project.activeRecipeId === reviewing.id}
          onClose={() => setReviewingId(null)}
          onSetActive={() =>
            run((current) => {
              const result = activateRecipeInState(current, reviewing.id);
              if (result.ok) setReviewingId(null);
              return result;
            })
          }
        />
      ) : null}
    </div>
  );
}
