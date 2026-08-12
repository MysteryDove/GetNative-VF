import type { ProjectState, Recipe } from "./types";
import {
  activateRecipeInState,
  activeRecipe,
  createRecipe,
  updateRecipe,
  type RecipeOpResult,
  type RecipePayloadPatch,
} from "./recipe";

/**
 * Apply an analysis-side payload (geometry/kernel/metric/…) to the current
 * Recipe: the pointed Recipe, the most recently edited one otherwise (adopted
 * as current), or a new one created with `defaultName`.
 */

/** Fallback apply target when no Recipe is current. */
export function mostRecentRecipe(state: ProjectState): Recipe | null {
  return (
    Object.values(state.recipesById).sort((a, b) =>
      b.updatedAt.localeCompare(a.updatedAt),
    )[0] ?? null
  );
}

export function applyPayloadToCurrentRecipe(
  state: ProjectState,
  patch: RecipePayloadPatch,
  labels: ApplyLabels,
  nowIso: string = new Date().toISOString(),
): { ok: true; state: ProjectState; recipe: Recipe } | { ok: false; reason: string } {
  // The current Recipe is the apply target; otherwise adopt the most recent
  // one (so the UI strip always shows the next apply's destination) or create.
  const target = activeRecipe(state) ?? mostRecentRecipe(state);
  const applied = !target
    ? createRecipe(
        state,
        { ...patch, name: patch.name?.trim() ? patch.name.trim() : labels.defaultName },
        undefined,
        nowIso,
      )
    : updateRecipe(state, target.id, patch, nowIso);
  if (!applied.ok) return applied;
  const named = withDerivedName(applied, labels);
  if (target && state.project.activeRecipeId !== target.id) {
    const activated = activateRecipeInState(named.state, target.id);
    return activated.ok ? { ...named, state: activated.state } : named;
  }
  return named;
}

/** Locale text the apply layer needs (model code stays translation-free). */
export type ApplyLabels = {
  defaultName: string;
  unknownKernel: string;
  unknownSize: string;
};

/**
 * Auto-naming: once a Recipe owns geometry or kernel content, its name
 * derives from it — `<canvasW>x<canvasH>-<kernelId>(k=v,…)`, with the missing
 * side rendered as the localized unknown label (e.g. `1440x810-未定义算法`)
 * and the Recipe's optional name suffix appended (`…-p2`). Brand-new empty
 * Recipes keep their initial name. Collisions with any other Recipe get a
 * numeric suffix.
 */
function withDerivedName(
  result: { ok: true; state: ProjectState; recipe: Recipe },
  labels: ApplyLabels,
): { ok: true; state: ProjectState; recipe: Recipe } {
  const base = derivedRecipeName(result.recipe, labels);
  if (!base) return result;
  const suffix = result.recipe.nameSuffix?.trim();
  const name = uniqueRecipeName(
    result.state,
    suffix ? `${base}-${suffix}` : base,
    result.recipe.id,
  );
  if (name === result.recipe.name) return result;
  const renamed = updateRecipe(result.state, result.recipe.id, { name });
  return renamed.ok ? renamed : result;
}

/**
 * Set the Recipe's name suffix (trimmed; empty clears) and re-derive its
 * name immediately so the strip always shows the current composition.
 */
export function setRecipeNameSuffix(
  state: ProjectState,
  recipeId: string,
  nameSuffix: string,
  labels: ApplyLabels,
  nowIso: string = new Date().toISOString(),
): RecipeOpResult {
  const updated = updateRecipe(state, recipeId, { nameSuffix: nameSuffix.trim() || null }, nowIso);
  if (!updated.ok) return updated;
  return withDerivedName(updated, labels);
}

export function derivedRecipeName(recipe: Recipe, labels: ApplyLabels): string | null {
  if (!recipe.geometry && !recipe.kernel?.id) return null;
  const size = recipe.geometry
    ? `${recipe.geometry.canvasWidth}x${recipe.geometry.canvasHeight}`
    : labels.unknownSize;
  let kernel = labels.unknownKernel;
  if (recipe.kernel?.id) {
    const params = Object.entries(recipe.kernel.parameters ?? {});
    kernel = params.length
      ? `${recipe.kernel.id}(${params.map(([key, value]) => `${key}=${value}`).join(",")})`
      : recipe.kernel.id;
  }
  return `${size}-${kernel}`;
}

function uniqueRecipeName(state: ProjectState, base: string, excludeId: string): string {
  const taken = new Set(
    Object.values(state.recipesById)
      .filter((recipe) => recipe.id !== excludeId)
      .map((recipe) => recipe.name),
  );
  if (!taken.has(base)) return base;
  for (let suffix = 2; ; suffix += 1) {
    const candidate = `${base}-${suffix}`;
    if (!taken.has(candidate)) return candidate;
  }
}
