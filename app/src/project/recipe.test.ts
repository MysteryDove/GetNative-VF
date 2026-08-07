import { describe, expect, it } from "vitest";
import { emptyProjectState } from "./normalize";
import {
  activateRecipeInState,
  createRecipeDraft,
  deactivateRecipeInState,
  deriveRecipeDraft,
  lockRecipeInState,
  recipeLockReadiness,
  removeRecipeInState,
  updateRecipeDraft,
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
  it("creates a mutable draft and locks it when complete", () => {
    const state = emptyProjectState({ id: "p1" });
    const created = createRecipeDraft(state, fullPayload(), sequencedIds(), "2026-08-07T00:00:00Z");
    expect(created.ok).toBe(true);
    if (!created.ok) return;
    expect(created.recipe.status).toBe("draft");
    expect(created.recipe.locked).toBe(false);
    expect(recipeLockReadiness(created.recipe).ok).toBe(true);

    const locked = lockRecipeInState(created.state, created.recipe.id, "2026-08-07T01:00:00Z");
    expect(locked.ok).toBe(true);
    if (!locked.ok) return;
    expect(locked.recipe.status).toBe("locked");
    expect(locked.recipe.locked).toBe(true);

    // Locked recipes are immutable.
    const mutated = updateRecipeDraft(locked.state, locked.recipe.id, { name: "Renamed" });
    expect(mutated.ok).toBe(false);
    if (!mutated.ok) expect(mutated.reason).toBe("recipe_immutable");
  });

  it("refuses to lock an incomplete draft and reports missing semantics", () => {
    const state = emptyProjectState({ id: "p1" });
    const created = createRecipeDraft(state, { name: "Partial" }, sequencedIds());
    expect(created.ok).toBe(true);
    if (!created.ok) return;
    const readiness = recipeLockReadiness(created.recipe);
    expect(readiness.ok).toBe(false);
    if (readiness.ok) return;
    expect(readiness.missing).toContain("geometry");
    expect(readiness.missing).toContain("kernel");
    expect(readiness.missing).toContain("metric");
    expect(readiness.missing).toContain("profile");
    expect(readiness.missing).toContain("math_mode");

    const locked = lockRecipeInState(created.state, created.recipe.id);
    expect(locked.ok).toBe(false);
    if (!locked.ok) expect(locked.reason).toBe("recipe_incomplete");
  });

  it("derives a new revision draft from a locked recipe and supersedes the parent on lock", () => {
    let state = emptyProjectState({ id: "p1" });
    const ids = sequencedIds();
    const created = createRecipeDraft(state, fullPayload(), ids);
    if (!created.ok) throw new Error("setup");
    const locked = lockRecipeInState(created.state, created.recipe.id);
    if (!locked.ok) throw new Error("setup");
    state = locked.state;

    const derived = deriveRecipeDraft(state, locked.recipe.id, ids);
    expect(derived.ok).toBe(true);
    if (!derived.ok) return;
    expect(derived.recipe.revision).toBe(2);
    expect(derived.recipe.parentRecipeId).toBe(locked.recipe.id);
    expect(derived.recipe.status).toBe("draft");

    const relocked = lockRecipeInState(derived.state, derived.recipe.id);
    expect(relocked.ok).toBe(true);
    if (!relocked.ok) return;
    expect(relocked.state.recipesById[locked.recipe.id]?.status).toBe("superseded");
    expect(relocked.state.recipesById[locked.recipe.id]?.locked).toBe(true);
    expect(relocked.recipe.status).toBe("locked");
  });

  it("activates only locked recipes and replaces the pointer atomically", () => {
    let state = emptyProjectState({ id: "p1" });
    const ids = sequencedIds();
    const first = createRecipeDraft(state, fullPayload(), ids);
    if (!first.ok) throw new Error("setup");
    const firstLocked = lockRecipeInState(first.state, first.recipe.id);
    if (!firstLocked.ok) throw new Error("setup");
    state = firstLocked.state;

    const second = createRecipeDraft(state, { ...fullPayload(), name: "Lanczos 720p" }, ids);
    if (!second.ok) throw new Error("setup");
    state = second.state;
    // Draft cannot be activated.
    expect(activateRecipeInState(state, second.recipe.id).ok).toBe(false);

    const activeFirst = activateRecipeInState(state, first.recipe.id);
    expect(activeFirst.ok).toBe(true);
    if (!activeFirst.ok) return;
    expect(activeFirst.state.project.activeRecipeId).toBe(first.recipe.id);

    const secondLocked = lockRecipeInState(activeFirst.state, second.recipe.id);
    if (!secondLocked.ok) throw new Error("setup");
    // Locking must not silently activate.
    expect(secondLocked.state.project.activeRecipeId).toBe(first.recipe.id);

    const activeSecond = activateRecipeInState(secondLocked.state, second.recipe.id);
    expect(activeSecond.ok).toBe(true);
    if (!activeSecond.ok) return;
    expect(activeSecond.state.project.activeRecipeId).toBe(second.recipe.id);

    // Superseded recipes cannot be newly activated.
    const derived = deriveRecipeDraft(activeSecond.state, first.recipe.id, ids);
    if (!derived.ok) throw new Error("setup");
    const supersededParent = lockRecipeInState(derived.state, derived.recipe.id);
    if (!supersededParent.ok) throw new Error("setup");
    expect(supersededParent.state.recipesById[first.recipe.id]?.status).toBe("superseded");
    expect(activateRecipeInState(supersededParent.state, first.recipe.id).ok).toBe(false);
  });

  it("blocks removing the active recipe until deactivation", () => {
    let state = emptyProjectState({ id: "p1" });
    const created = createRecipeDraft(state, fullPayload(), sequencedIds());
    if (!created.ok) throw new Error("setup");
    const locked = lockRecipeInState(created.state, created.recipe.id);
    if (!locked.ok) throw new Error("setup");
    const activated = activateRecipeInState(locked.state, locked.recipe.id);
    if (!activated.ok) throw new Error("setup");
    state = activated.state;

    const blocked = removeRecipeInState(state, locked.recipe.id);
    expect(blocked.ok).toBe(false);
    if (!blocked.ok) expect(blocked.reason).toBe("recipe_active");

    state = deactivateRecipeInState(state);
    const removed = removeRecipeInState(state, locked.recipe.id);
    expect(removed.ok).toBe(true);
    if (!removed.ok) return;
    expect(removed.state.recipesById[locked.recipe.id]).toBeUndefined();
  });

  it("rejects invalid metric specs at lock time", () => {
    const state = emptyProjectState({ id: "p1" });
    const created = createRecipeDraft(
      state,
      { ...fullPayload(), metric: { ...metric, pNorm: 0 } },
      sequencedIds(),
    );
    if (!created.ok) throw new Error("setup");
    const readiness = recipeLockReadiness(created.recipe);
    expect(readiness.ok).toBe(false);
    if (readiness.ok) return;
    expect(readiness.missing).toContain("metric_invalid");
  });
});
