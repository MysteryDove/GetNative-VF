import { describe, expect, it } from "vitest";
import { emptyProjectState } from "./normalize";
import {
  activateRecipeInState,
  activeRecipe,
  createRecipe,
  deactivateRecipeInState,
  recipeReadiness,
  removeRecipeInState,
  updateRecipe,
} from "./recipe";
import type { GeometrySnapshot, MetricSpec } from "../engine/protocol";

const geometry: GeometrySnapshot = {
  mode: "standard",
  activeWidth: 1920,
  activeHeight: 1080,
  canvasWidth: 1920,
  canvasHeight: 1080,
  srcLeft: 0,
  srcTop: 0,
  srcWidth: 1920,
  srcHeight: 1080,
  baseHeight: 720,
  baseWidth: null,
  parity: null,
};

const metric: MetricSpec = {
  cropLeft: 10,
  cropRight: 10,
  cropTop: 10,
  cropBottom: 10,
  pixelExclusionThreshold: 0.015,
  pNorm: 1,
};

function fullPayload() {
  return {
    name: "Bicubic 720p",
    geometry,
    kernel: { id: "bicubic", parameters: { b: 0, c: 0.5 } },
    metric,
    profileId: "muf-d278cd3",
    mathMode: "raw" as const,
  };
}

function sequencedIds(): () => string {
  let counter = 0;
  return () => `test_${counter++}`;
}

describe("recipe domain", () => {
  it("creates a recipe and makes it current", () => {
    const state = emptyProjectState({ id: "p1" });
    const created = createRecipe(state, fullPayload(), sequencedIds(), "2026-08-07T00:00:00Z");
    expect(created.ok).toBe(true);
    if (!created.ok) return;
    expect(created.state.project.activeRecipeId).toBe(created.recipe.id);
    expect(recipeReadiness(created.recipe).ok).toBe(true);
  });

  it("updates any recipe in place", () => {
    const state = emptyProjectState({ id: "p1" });
    const created = createRecipe(state, { name: "Partial" }, sequencedIds());
    if (!created.ok) throw new Error("setup");
    const updated = updateRecipe(created.state, created.recipe.id, { geometry });
    expect(updated.ok).toBe(true);
    if (!updated.ok) return;
    expect(updated.recipe.geometry?.canvasHeight).toBe(1080);
    expect(updated.state.recipesById[created.recipe.id]?.geometry).not.toBeNull();
    const missing = updateRecipe(updated.state, "recipe_ghost", { geometry });
    expect(missing.ok).toBe(false);
    if (!missing.ok) expect(missing.reason).toBe("recipe_missing");
  });

  it("reports missing semantics and rejects invalid metric specs", () => {
    const state = emptyProjectState({ id: "p1" });
    const created = createRecipe(state, { name: "Partial" }, sequencedIds());
    if (!created.ok) throw new Error("setup");
    const readiness = recipeReadiness(created.recipe);
    expect(readiness.ok).toBe(false);
    if (readiness.ok) return;
    expect(readiness.missing).toContain("geometry");
    expect(readiness.missing).toContain("kernel");
    expect(readiness.missing).toContain("metric");
    expect(readiness.missing).toContain("profile");
    expect(readiness.missing).toContain("math_mode");

    const invalid = createRecipe(
      state,
      { ...fullPayload(), metric: { ...metric, pNorm: 0 } },
      sequencedIds(),
    );
    if (!invalid.ok) throw new Error("setup");
    const invalidReadiness = recipeReadiness(invalid.recipe);
    expect(invalidReadiness.ok).toBe(false);
    if (invalidReadiness.ok) return;
    expect(invalidReadiness.missing).toContain("metric_invalid");
  });

  it("blocks a Recipe whose migrated geometry needs review", () => {
    const state = emptyProjectState({ id: "p1" });
    const created = createRecipe(
      state,
      { ...fullPayload(), geometry: { ...geometry, needsReview: true } },
      sequencedIds(),
    );
    if (!created.ok) throw new Error("setup");
    const result = recipeReadiness(created.recipe);
    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.missing).toContain("geometry_review");
  });

  it("activates any recipe as current and deactivates the pointer", () => {
    const state = emptyProjectState({ id: "p1" });
    const first = createRecipe(state, fullPayload(), sequencedIds());
    if (!first.ok) throw new Error("setup");
    const second = createRecipe(first.state, { name: "B" }, sequencedIds());
    if (!second.ok) throw new Error("setup");
    // Creation moved the pointer to B; activating A replaces it.
    const activated = activateRecipeInState(second.state, first.recipe.id);
    expect(activated.ok).toBe(true);
    if (!activated.ok) return;
    expect(activeRecipe(activated.state)?.id).toBe(first.recipe.id);

    const missing = activateRecipeInState(activated.state, "recipe_ghost");
    expect(missing.ok).toBe(false);
    if (!missing.ok) expect(missing.reason).toBe("recipe_missing");

    const cleared = deactivateRecipeInState(activated.state);
    expect(cleared.project.activeRecipeId).toBeNull();
  });

  it("blocks removing the current recipe and removes others", () => {
    const state = emptyProjectState({ id: "p1" });
    const ids = sequencedIds();
    const first = createRecipe(state, fullPayload(), ids);
    if (!first.ok) throw new Error("setup");
    const blocked = removeRecipeInState(first.state, first.recipe.id);
    expect(blocked.ok).toBe(false);
    if (!blocked.ok) expect(blocked.reason).toBe("recipe_active");

    const second = createRecipe(first.state, { name: "Scratch" }, ids);
    if (!second.ok) throw new Error("setup");
    const removed = removeRecipeInState(second.state, first.recipe.id);
    expect(removed.ok).toBe(true);
    if (!removed.ok) return;
    expect(removed.state.recipesById[first.recipe.id]).toBeUndefined();
  });
});
