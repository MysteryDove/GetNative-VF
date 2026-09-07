import type { ProjectState, Recipe, Run } from "./types";

function record(value: unknown): Record<string, unknown> | null {
  return value && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown> : null;
}

/** A completed measurement must not depend on the current mutable recipe. */
export function verificationRecipe(run: Run, state: Pick<ProjectState, "recipesById">): Recipe | null {
  const input = record(run.inputSnapshot);
  const request = record(input?.request) ?? input;
  if (typeof request?.recipeId !== "string" || !Number.isInteger(request.recipeRevision)) return null;
  const saved = record(input?.recipeSnapshot);
  if (saved) {
    if (saved.id !== request.recipeId || saved.revision !== request.recipeRevision
      || typeof saved.name !== "string" || typeof saved.createdAt !== "string"
      || !record(saved.geometry) || !record(saved.kernel) || !record(saved.metric)) return null;
    return saved as Recipe;
  }
  const current = state.recipesById[request.recipeId];
  if (!current || current.revision !== request.recipeRevision) return null;
  // Legacy runs already stored the measured values in their request. Use
  // those values when available; only metadata comes from the live recipe.
  return {
    ...current,
    geometry: (record(request.geometry) as Recipe["geometry"]) ?? current.geometry,
    kernel: (record(request.kernel) as Recipe["kernel"]) ?? current.kernel,
    metric: (record(request.metric) as Recipe["metric"]) ?? current.metric,
    axisMode: (request.axisMode as Recipe["axisMode"]) ?? current.axisMode,
    profileId: (request.profileId as Recipe["profileId"]) ?? current.profileId,
    mathMode: (request.mathMode as Recipe["mathMode"]) ?? current.mathMode,
  };
}

/** Capture legacy metadata before editing or deleting its only remaining copy. */
export function preserveVerificationRecipes(state: ProjectState, recipeId: string): ProjectState {
  let runsById = state.runsById;
  for (const run of Object.values(state.runsById)) {
    if (run.runType !== "verification" && run.runType !== "verify") continue;
    const input = record(run.inputSnapshot);
    const request = record(input?.request) ?? input;
    if (request?.recipeId !== recipeId || input?.recipeSnapshot) continue;
    const recipe = verificationRecipe(run, state);
    if (!recipe) continue;
    if (runsById === state.runsById) runsById = { ...runsById };
    runsById[run.id] = { ...run, inputSnapshot: { ...input, recipeSnapshot: structuredClone(recipe) } };
  }
  return runsById === state.runsById ? state : { ...state, runsById };
}
