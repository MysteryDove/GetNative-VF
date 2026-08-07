import { useState } from "react";
import type { Translator } from "../i18n";
import {
  activateRecipeInState,
  deactivateRecipeInState,
  deriveRecipeDraft,
  lockRecipeInState,
  removeRecipeInState,
  updateRecipeDraft,
} from "../project/recipe";
import type { ProjectState, Recipe } from "../project/types";
import { RecipeReviewDialog } from "./RecipeReviewDialog";

/**
 * Recipe Manager: the whole Recipe lifecycle in one place — draft review and
 * lock, atomic activation, revision derivation, guarded removal. Rendered on
 * the Project Overview, which owns Recipe activation per the route matrix.
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

  const recipes = Object.values(state.recipesById).sort((a, b) =>
    b.updatedAt.localeCompare(a.updatedAt),
  );
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

  function statusLabel(recipe: Recipe): string {
    if (recipe.status === "locked") return t("recipe.locked");
    if (recipe.status === "superseded") return t("recipe.superseded");
    return t("recipe.draft");
  }

  return (
    <div className="recipe-manager">
      {recipes.length === 0 ? (
        <p className="empty-copy">{t("overview.recipesEmpty")}</p>
      ) : (
        <div className="dense-table">
          {recipes.map((recipe) => {
            const isActive = state.project.activeRecipeId === recipe.id;
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
                  {" · "}
                  {statusLabel(recipe)}
                  {recipe.updatedAt ? ` · ${recipe.updatedAt.slice(0, 10)}` : ""}
                </span>
                <span className="recipe-actions">
                  {recipe.status === "draft" ? (
                    <button
                      className="link-button"
                      type="button"
                      onClick={() => setReviewingId(recipe.id)}
                    >
                      {t("recipe.review")}
                    </button>
                  ) : null}
                  {recipe.status === "locked" && !isActive ? (
                    <button
                      className="link-button"
                      type="button"
                      onClick={() =>
                        run((current) => activateRecipeInState(current, recipe.id))
                      }
                    >
                      {t("recipe.activate")}
                    </button>
                  ) : null}
                  {isActive ? (
                    <button
                      className="link-button"
                      type="button"
                      onClick={() =>
                        run((current) => ({ ok: true, state: deactivateRecipeInState(current) }))
                      }
                    >
                      {t("recipe.deactivate")}
                    </button>
                  ) : null}
                  {recipe.status !== "draft" ? (
                    <>
                      <button
                        className="link-button"
                        type="button"
                        onClick={() => setReviewingId(recipe.id)}
                      >
                        {t("recipe.inspect")}
                      </button>
                      <button
                        className="link-button"
                        type="button"
                        onClick={() =>
                          run((current) => deriveRecipeDraft(current, recipe.id))
                        }
                      >
                        {t("recipe.derive")}
                      </button>
                    </>
                  ) : null}
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
          onClose={() => setReviewingId(null)}
          onRename={(name) =>
            run((current) => updateRecipeDraft(current, reviewing.id, { name }))
          }
          onLock={() =>
            run((current) => {
              const result = lockRecipeInState(current, reviewing.id);
              if (result.ok) setReviewingId(null);
              return result;
            })
          }
          onActivate={() =>
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
