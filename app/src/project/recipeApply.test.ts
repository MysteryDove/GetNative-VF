import { describe, expect, it } from "vitest";
import { emptyProjectState } from "./normalize";
import {
  applyPayloadToCurrentRecipe,
  mostRecentRecipe,
  setRecipeNameSuffix,
  type ApplyLabels,
} from "./recipeApply";
import { activateRecipeInState, createRecipe, deactivateRecipeInState } from "./recipe";
import type { GeometrySnapshot, MetricSpec } from "../engine/protocol";

const NOW = "2026-08-08T00:00:00Z";

const LABELS: ApplyLabels = {
  defaultName: "未命名方案",
  unknownKernel: "未定义算法",
  unknownSize: "未定义尺寸",
};

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

function fullPatch() {
  return {
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

describe("apply-to-current composition", () => {
  it("apply creates a recipe and makes it current on an empty project", () => {
    const state = emptyProjectState({ id: "p1" });
    const result = applyPayloadToCurrentRecipe(state, fullPatch(), LABELS, NOW);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.recipe.name).toBe("1920x1080-bicubic(b=0,c=0.5)");
    expect(result.state.project.activeRecipeId).toBe(result.recipe.id);
  });

  it("apply writes to the current recipe rather than the most recent one", () => {
    const state = emptyProjectState({ id: "p1" });
    const first = applyPayloadToCurrentRecipe(state, { geometry }, LABELS, NOW);
    if (!first.ok) throw new Error("setup");
    const second = createRecipe(first.state, { name: "方案B" }, sequencedIds(), NOW);
    if (!second.ok) throw new Error("setup");
    // Creation moved the pointer; go back to the first recipe.
    const activated = activateRecipeInState(second.state, first.recipe.id);
    if (!activated.ok) throw new Error("setup");

    const applied = applyPayloadToCurrentRecipe(
      activated.state,
      { kernel: { id: "lanczos", parameters: { taps: 3 } } },
      LABELS,
      NOW,
    );
    expect(applied.ok).toBe(true);
    if (!applied.ok) return;
    expect(applied.recipe.id).toBe(first.recipe.id);
    expect(applied.state.recipesById[second.recipe.id]?.kernel).toBeNull();
    expect(applied.state.project.activeRecipeId).toBe(first.recipe.id);
  });

  it("adopts the most recent recipe as current when the pointer is unset", () => {
    const state = emptyProjectState({ id: "p1" });
    const created = createRecipe(state, { name: "方案A" }, sequencedIds(), NOW);
    if (!created.ok) throw new Error("setup");
    const cleared = deactivateRecipeInState(created.state);

    const applied = applyPayloadToCurrentRecipe(
      cleared,
      { geometry, metric, profileId: "muf-d278cd3", mathMode: "raw" },
      LABELS,
      NOW,
    );
    expect(applied.ok).toBe(true);
    if (!applied.ok) return;
    expect(applied.recipe.id).toBe(created.recipe.id);
    expect(mostRecentRecipe(applied.state)?.id).toBe(created.recipe.id);
    expect(applied.state.project.activeRecipeId).toBe(created.recipe.id);
  });
});

describe("derived recipe naming", () => {
  it("keeps the initial name while geometry and kernel are both absent", () => {
    const state = emptyProjectState({ id: "p1" });
    const staged = applyPayloadToCurrentRecipe(
      state,
      { metric, profileId: "muf-d278cd3", mathMode: "raw" },
      LABELS,
      NOW,
    );
    if (!staged.ok) throw new Error("setup");
    expect(staged.recipe.name).toBe("未命名方案");
  });

  it("derives with an unknown-kernel placeholder on a geometry-only apply", () => {
    const state = emptyProjectState({ id: "p1" });
    const staged = applyPayloadToCurrentRecipe(state, { geometry }, LABELS, NOW);
    expect(staged.ok).toBe(true);
    if (!staged.ok) return;
    expect(staged.recipe.name).toBe("1920x1080-未定义算法");

    const completed = applyPayloadToCurrentRecipe(
      staged.state,
      { kernel: { id: "lanczos", parameters: { taps: 3 } } },
      LABELS,
      NOW,
    );
    expect(completed.ok).toBe(true);
    if (!completed.ok) return;
    expect(completed.recipe.name).toBe("1920x1080-lanczos(taps=3)");
  });

  it("derives with an unknown-size placeholder on a kernel-only apply", () => {
    const state = emptyProjectState({ id: "p1" });
    const staged = applyPayloadToCurrentRecipe(
      state,
      { kernel: { id: "spline36", parameters: {} } },
      LABELS,
      NOW,
    );
    expect(staged.ok).toBe(true);
    if (!staged.ok) return;
    expect(staged.recipe.name).toBe("未定义尺寸-spline36");
  });

  it("suffixes the derived name when another recipe already owns it", () => {
    const state = emptyProjectState({ id: "p1" });
    const first = applyPayloadToCurrentRecipe(state, fullPatch(), LABELS, NOW);
    if (!first.ok) throw new Error("setup");
    const another = createRecipe(first.state, { name: "方案B" }, sequencedIds(), NOW);
    if (!another.ok) throw new Error("setup");
    const collided = applyPayloadToCurrentRecipe(another.state, fullPatch(), LABELS, NOW);
    expect(collided.ok).toBe(true);
    if (!collided.ok) return;
    expect(collided.recipe.id).toBe(another.recipe.id);
    expect(collided.recipe.name).toBe("1920x1080-bicubic(b=0,c=0.5)-2");
  });

  it("re-derives the name when the kernel changes on the same recipe", () => {
    const state = emptyProjectState({ id: "p1" });
    const staged = applyPayloadToCurrentRecipe(state, fullPatch(), LABELS, NOW);
    if (!staged.ok) throw new Error("setup");
    expect(staged.recipe.name).toBe("1920x1080-bicubic(b=0,c=0.5)");
    const swapped = applyPayloadToCurrentRecipe(
      staged.state,
      { kernel: { id: "spline36", parameters: {} } },
      LABELS,
      NOW,
    );
    expect(swapped.ok).toBe(true);
    if (!swapped.ok) return;
    expect(swapped.recipe.name).toBe("1920x1080-spline36");
  });

  it("appends the name suffix and re-derives immediately on suffix change", () => {
    const state = emptyProjectState({ id: "p1" });
    const staged = applyPayloadToCurrentRecipe(state, fullPatch(), LABELS, NOW);
    if (!staged.ok) throw new Error("setup");
    expect(staged.recipe.name).toBe("1920x1080-bicubic(b=0,c=0.5)");

    const suffixed = setRecipeNameSuffix(staged.state, staged.recipe.id, " p2 ", LABELS, NOW);
    expect(suffixed.ok).toBe(true);
    if (!suffixed.ok) return;
    expect(suffixed.recipe.nameSuffix).toBe("p2");
    expect(suffixed.recipe.name).toBe("1920x1080-bicubic(b=0,c=0.5)-p2");

    // Subsequent applies keep the suffix.
    const reapplied = applyPayloadToCurrentRecipe(
      suffixed.state,
      { kernel: { id: "spline36", parameters: {} } },
      LABELS,
      NOW,
    );
    expect(reapplied.ok).toBe(true);
    if (!reapplied.ok) return;
    expect(reapplied.recipe.name).toBe("1920x1080-spline36-p2");

    // Clearing the suffix restores the bare derived name.
    const cleared = setRecipeNameSuffix(reapplied.state, staged.recipe.id, "", LABELS, NOW);
    expect(cleared.ok).toBe(true);
    if (!cleared.ok) return;
    expect(cleared.recipe.nameSuffix).toBeNull();
    expect(cleared.recipe.name).toBe("1920x1080-spline36");
  });
});
