import type {
  GeometrySnapshot,
  AxisMode,
  KernelRef,
  MathMode,
  MetricSpec,
} from "../engine/protocol";
import { validateMetricSpec } from "../engine/shapeGuards";
import { profileFor } from "../engine/profiles";
import type { ProjectState, Recipe } from "./types";

/**
 * Recipe domain operations. All functions are pure ProjectState transforms so
 * they can be tested directly and plugged into the shell's `onProjectChange`.
 *
 * Single-current semantics: a Project holds any number of Recipes, all
 * mutable; `active_recipe_id` points at the current one — the apply target
 * for analysis pages and the input for whole-video Verification. Verification
 * gating is field completeness (`recipeReadiness`), not a lock ceremony.
 * The persisted `status`/`revision` fields remain for schema compatibility
 * with older manifests and are no longer consulted.
 */

export type RecipePayloadPatch = {
  name?: string;
  nameSuffix?: string | null;
  geometry?: GeometrySnapshot | null;
  kernel?: KernelRef | null;
  metric?: MetricSpec | null;
  axisMode?: AxisMode;
  profileId?: string | null;
  mathMode?: MathMode | null;
};

export type RecipeOpResult =
  | { ok: true; state: ProjectState; recipe: Recipe }
  | { ok: false; reason: string };

function defaultIdFactory(): string {
  return crypto.randomUUID();
}

export function createRecipe(
  state: ProjectState,
  input: RecipePayloadPatch & { name: string },
  idFactory: () => string = defaultIdFactory,
  nowIso: string = new Date().toISOString(),
): RecipeOpResult {
  const name = input.name.trim();
  if (!name) return { ok: false, reason: "recipe_name_required" };
  const recipe: Recipe = {
    id: `recipe_${idFactory()}`,
    name,
    nameSuffix: input.nameSuffix ?? null,
    status: "draft",
    locked: false,
    revision: 1,
    parentRecipeId: null,
    createdAt: nowIso,
    updatedAt: nowIso,
    geometry: input.geometry ?? null,
    kernel: input.kernel ?? null,
    metric: input.metric ?? null,
    axisMode: input.axisMode ?? profileFor(input.profileId ?? "").default_axis_mode,
    profileId: input.profileId ?? null,
    mathMode: input.mathMode ?? null,
  };
  return {
    ok: true,
    state: {
      ...state,
      project: { ...state.project, activeRecipeId: recipe.id },
      recipesById: { ...state.recipesById, [recipe.id]: recipe },
    },
    recipe,
  };
}

/** Every Recipe is mutable; updates always land in place. */
export function updateRecipe(
  state: ProjectState,
  recipeId: string,
  patch: RecipePayloadPatch,
  nowIso: string = new Date().toISOString(),
): RecipeOpResult {
  const recipe = state.recipesById[recipeId];
  if (!recipe) return { ok: false, reason: "recipe_missing" };
  const next: Recipe = {
    ...recipe,
    ...(patch.name !== undefined ? { name: patch.name } : {}),
    ...(patch.nameSuffix !== undefined ? { nameSuffix: patch.nameSuffix } : {}),
    ...(patch.geometry !== undefined ? { geometry: patch.geometry } : {}),
    ...(patch.kernel !== undefined ? { kernel: patch.kernel } : {}),
    ...(patch.metric !== undefined ? { metric: patch.metric } : {}),
    ...(patch.axisMode !== undefined ? { axisMode: patch.axisMode } : {}),
    ...(patch.profileId !== undefined ? { profileId: patch.profileId } : {}),
    ...(patch.mathMode !== undefined ? { mathMode: patch.mathMode } : {}),
    updatedAt: nowIso,
  };
  return {
    ok: true,
    state: { ...state, recipesById: { ...state.recipesById, [recipeId]: next } },
    recipe: next,
  };
}

export type RecipeReadiness = { ok: true } | { ok: false; missing: string[] };

/**
 * A Recipe can feed whole-video Verification when every metric-changing
 * semantic value is present and valid (DESIGN.md: confirmed geometry and
 * kernel plus all metric/crop parameters).
 */
export function recipeReadiness(recipe: Recipe): RecipeReadiness {
  const missing: string[] = [];
  if (!recipe.name.trim()) missing.push("name");
  if (!recipe.geometry) missing.push("geometry");
  else if (recipe.geometry.needsReview) missing.push("geometry_review");
  if (!recipe.kernel?.id) missing.push("kernel");
  if (!recipe.metric) {
    missing.push("metric");
  } else if (!validateMetricSpec(recipe.metric).ok) {
    missing.push("metric_invalid");
  }
  if (!recipe.profileId) missing.push("profile");
  if (!recipe.mathMode) missing.push("math_mode");
  return missing.length ? { ok: false, missing } : { ok: true };
}

/**
 * Make a Recipe the current one: the only pointer is `active_recipe_id`;
 * replacing it is one atomic assignment. Any Recipe can be current.
 */
export function activateRecipeInState(
  state: ProjectState,
  recipeId: string,
): RecipeOpResult {
  const recipe = state.recipesById[recipeId];
  if (!recipe) return { ok: false, reason: "recipe_missing" };
  return {
    ok: true,
    state: {
      ...state,
      project: { ...state.project, activeRecipeId: recipeId },
    },
    recipe,
  };
}

export function deactivateRecipeInState(state: ProjectState): ProjectState {
  if (!state.project.activeRecipeId) return state;
  return { ...state, project: { ...state.project, activeRecipeId: null } };
}

/** The current Recipe must be deactivated or replaced before removal. */
export function removeRecipeInState(
  state: ProjectState,
  recipeId: string,
): { ok: true; state: ProjectState } | { ok: false; reason: string } {
  const recipe = state.recipesById[recipeId];
  if (!recipe) return { ok: false, reason: "recipe_missing" };
  if (state.project.activeRecipeId === recipeId) {
    return { ok: false, reason: "recipe_active" };
  }
  const recipesById = { ...state.recipesById };
  delete recipesById[recipeId];
  return { ok: true, state: { ...state, recipesById } };
}

export function activeRecipe(state: ProjectState): Recipe | null {
  const id = state.project.activeRecipeId;
  return id ? (state.recipesById[id] ?? null) : null;
}

/** All Recipes sorted by `updatedAt` descending (most recently touched first). */
export function recipesByUpdatedAt(state: ProjectState): Recipe[] {
  return Object.values(state.recipesById).sort((a, b) =>
    b.updatedAt.localeCompare(a.updatedAt),
  );
}
