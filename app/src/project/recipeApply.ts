import type { ProjectState, Recipe } from "./types";
import { createRecipeDraft, updateRecipeDraft, type RecipePayloadPatch } from "./recipe";

/**
 * Apply an analysis-side payload (geometry/kernel/metric/…) to the current
 * Recipe Draft: the most recently edited existing draft, or a new one created
 * with `defaultName`. Applying never touches locked Recipes and never
 * activates anything.
 */
export function applyPayloadToRecipeDraft(
  state: ProjectState,
  patch: RecipePayloadPatch,
  defaultName: string,
  nowIso: string = new Date().toISOString(),
): { ok: true; state: ProjectState; recipe: Recipe } | { ok: false; reason: string } {
  const drafts = Object.values(state.recipesById)
    .filter((recipe) => recipe.status === "draft")
    .sort((a, b) => b.updatedAt.localeCompare(a.updatedAt));
  const target = drafts[0];
  if (!target) {
    return createRecipeDraft(
      state,
      { ...patch, name: patch.name?.trim() ? patch.name.trim() : defaultName },
      undefined,
      nowIso,
    );
  }
  return updateRecipeDraft(state, target.id, patch, nowIso);
}
