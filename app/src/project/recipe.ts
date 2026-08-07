import type {
  GeometrySnapshot,
  KernelRef,
  MathMode,
  MetricSpec,
} from "../engine/protocol";
import { validateMetricSpec } from "../engine/shapeGuards";
import type { ProjectState, Recipe } from "./types";

/**
 * Recipe domain operations. All functions are pure ProjectState transforms so
 * they can be tested directly and plugged into the shell's `onProjectChange`.
 *
 * Lifecycle (DESIGN.md): draft → locked → (superseded when a derived revision
 * locks). Locking never silently activates; activation only ever points at a
 * locked Recipe and replaces the prior pointer atomically.
 */

export type RecipePayloadPatch = {
  name?: string;
  geometry?: GeometrySnapshot | null;
  kernel?: KernelRef | null;
  metric?: MetricSpec | null;
  profileId?: string | null;
  mathMode?: MathMode | null;
};

export type RecipeOpResult =
  | { ok: true; state: ProjectState; recipe: Recipe }
  | { ok: false; reason: string };

function defaultIdFactory(): string {
  return crypto.randomUUID();
}

export function createRecipeDraft(
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
    status: "draft",
    locked: false,
    revision: 1,
    parentRecipeId: null,
    createdAt: nowIso,
    updatedAt: nowIso,
    geometry: input.geometry ?? null,
    kernel: input.kernel ?? null,
    metric: input.metric ?? null,
    profileId: input.profileId ?? null,
    mathMode: input.mathMode ?? null,
  };
  return {
    ok: true,
    state: {
      ...state,
      recipesById: { ...state.recipesById, [recipe.id]: recipe },
    },
    recipe,
  };
}

/** Drafts are mutable; locked/superseded Recipes never change in place. */
export function updateRecipeDraft(
  state: ProjectState,
  recipeId: string,
  patch: RecipePayloadPatch,
  nowIso: string = new Date().toISOString(),
): RecipeOpResult {
  const recipe = state.recipesById[recipeId];
  if (!recipe) return { ok: false, reason: "recipe_missing" };
  if (recipe.status !== "draft") return { ok: false, reason: "recipe_immutable" };
  const next: Recipe = {
    ...recipe,
    ...(patch.name !== undefined ? { name: patch.name } : {}),
    ...(patch.geometry !== undefined ? { geometry: patch.geometry } : {}),
    ...(patch.kernel !== undefined ? { kernel: patch.kernel } : {}),
    ...(patch.metric !== undefined ? { metric: patch.metric } : {}),
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

export type LockReadiness = { ok: true } | { ok: false; missing: string[] };

/**
 * A Recipe can lock only when every metric-changing semantic value is present
 * and valid (DESIGN.md: "Lock Recipe requires confirmed geometry and kernel
 * plus all metric/crop parameters").
 */
export function recipeLockReadiness(recipe: Recipe): LockReadiness {
  const missing: string[] = [];
  if (!recipe.name.trim()) missing.push("name");
  if (!recipe.geometry) missing.push("geometry");
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
 * Lock a draft: it becomes immutable. If it was derived from a locked parent,
 * the parent becomes superseded (immutable history, no longer activatable).
 * Locking never changes `active_recipe_id`.
 */
export function lockRecipeInState(
  state: ProjectState,
  recipeId: string,
  nowIso: string = new Date().toISOString(),
): RecipeOpResult {
  const recipe = state.recipesById[recipeId];
  if (!recipe) return { ok: false, reason: "recipe_missing" };
  if (recipe.status !== "draft") return { ok: false, reason: "recipe_not_draft" };
  const readiness = recipeLockReadiness(recipe);
  if (!readiness.ok) return { ok: false, reason: "recipe_incomplete" };

  const locked: Recipe = {
    ...recipe,
    status: "locked",
    locked: true,
    updatedAt: nowIso,
  };
  const recipesById = { ...state.recipesById, [recipeId]: locked };

  const parent = recipe.parentRecipeId
    ? state.recipesById[recipe.parentRecipeId]
    : null;
  if (parent && parent.status === "locked") {
    recipesById[parent.id] = {
      ...parent,
      status: "superseded",
      locked: true,
      updatedAt: nowIso,
    };
  }
  return { ok: true, state: { ...state, recipesById }, recipe: locked };
}

/**
 * Edit a locked/superseded Recipe by deriving a new draft linked to it.
 * The derived draft carries the next revision number; locking it creates a
 * new Recipe (the original is never mutated).
 */
export function deriveRecipeDraft(
  state: ProjectState,
  recipeId: string,
  idFactory: () => string = defaultIdFactory,
  nowIso: string = new Date().toISOString(),
): RecipeOpResult {
  const source = state.recipesById[recipeId];
  if (!source) return { ok: false, reason: "recipe_missing" };
  if (source.status === "draft") return { ok: false, reason: "recipe_already_draft" };
  const draft: Recipe = {
    ...source,
    id: `recipe_${idFactory()}`,
    status: "draft",
    locked: false,
    revision: source.revision + 1,
    parentRecipeId: source.id,
    createdAt: nowIso,
    updatedAt: nowIso,
  };
  return {
    ok: true,
    state: {
      ...state,
      recipesById: { ...state.recipesById, [draft.id]: draft },
    },
    recipe: draft,
  };
}

/**
 * Activate a locked Recipe: the only activation pointer is
 * `active_recipe_id`; replacing it is one atomic assignment. Draft and
 * superseded Recipes cannot be newly activated.
 */
export function activateRecipeInState(
  state: ProjectState,
  recipeId: string,
): RecipeOpResult {
  const recipe = state.recipesById[recipeId];
  if (!recipe) return { ok: false, reason: "recipe_missing" };
  if (recipe.status !== "locked") return { ok: false, reason: "recipe_not_activatable" };
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

/** The active Recipe must be deactivated or replaced before removal. */
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
