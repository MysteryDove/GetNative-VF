import { describe, expect, it } from "vitest";
import {
  defaultVerifyDraft,
  extractVerifyFrames,
  planVerifyRunGroup,
  resolveScanScope,
} from "./verifyPlan";
import type { Recipe, Source } from "../project/types";

const completeRecipe: Recipe = {
  id: "recipe_1",
  name: "Bicubic 720p",
  status: "draft",
  locked: false,
  revision: 3,
  parentRecipeId: null,
  createdAt: "2026-08-01T00:00:00Z",
  updatedAt: "2026-08-01T00:00:00Z",
  geometry: {
    mode: "standard",
    activeWidth: 1920,
    activeHeight: 1080,
    canvasWidth: 1280,
    canvasHeight: 720,
    srcLeft: 0,
    srcTop: 0,
    srcWidth: 1920,
    srcHeight: 1080,
    baseWidth: null,
    baseHeight: 720,
    parity: null,
  },
  kernel: { id: "bicubic", parameters: { b: 0, c: 0.5 } },
  metric: {
    cropLeft: 10,
    cropRight: 10,
    cropTop: 10,
    cropBottom: 10,
    pixelExclusionThreshold: 0.015,
    pNorm: 1,
  },
  axisMode: "h_only",
  profileId: "muf-d278cd3",
  mathMode: "raw",
};

const video: Source = {
  id: "src_1",
  kind: "video",
  path: "/tmp/episode.mkv",
  fingerprint: "fp-1",
  state: "ready",
  videoStreams: [],
  selectedStreamIndex: 0,
  width: 1920,
  height: 1080,
};

describe("resolveScanScope", () => {
  it("defaults media verification concurrency to two", () => {
    expect(defaultVerifyDraft().concurrency).toBe(2);
  });
  it("builds a full-scan scope and validates ranges", () => {
    const full = resolveScanScope({ ...defaultVerifyDraft(), scopeKind: "full" }, 0);
    expect(full.ok).toBe(true);
    if (full.ok) expect(full.scope.selection).toBe("all");

    const bad = resolveScanScope(
      { ...defaultVerifyDraft(), scopeKind: "full", startFrame: "100", endFrame: "10" },
      0,
    );
    expect(bad.ok).toBe(false);
  });

  it("validates every-N and decoded-I-picture preview scopes", () => {
    const everyN = resolveScanScope(
      { ...defaultVerifyDraft(), scopeKind: "preview", previewRule: "every_n", everyN: "24" },
      0,
    );
    expect(everyN.ok).toBe(true);
    if (everyN.ok) {
      expect(everyN.scope.selection).toBe("every_n");
      expect(everyN.scope.everyN).toBe(24);
    }

    const invalid = resolveScanScope(
      { ...defaultVerifyDraft(), scopeKind: "preview", previewRule: "every_n", everyN: "0" },
      0,
    );
    expect(invalid.ok).toBe(false);
    if (!invalid.ok) expect(invalid.reason).toBe("verify_every_n_invalid");

    const iframe = resolveScanScope(
      { ...defaultVerifyDraft(), scopeKind: "preview", previewRule: "decoded_i_picture" },
      2,
    );
    expect(iframe.ok).toBe(true);
    if (iframe.ok) {
      expect(iframe.scope.selection).toBe("decoded_i_picture");
      expect(iframe.scope.streamIndex).toBe(2);
    }
  });
});

describe("planVerifyRunGroup", () => {
  it("creates one member VerificationRun per source with the recipe snapshot", () => {
    const draft = { ...defaultVerifyDraft(), scopeKind: "full" as const, sourceIds: ["src_1"] };
    const result = planVerifyRunGroup({
      draft,
      recipe: completeRecipe,
      sourcesById: { src_1: video },
      nowMs: 7,
      requestIdPrefix: "t",
    });
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.plan.groupType).toBe("single_verification");
    const member = result.plan.members[0];
    expect(member?.request.recipeId).toBe("recipe_1");
    expect(member?.request.recipeRevision).toBe(3);
    expect(member?.request.kernel.id).toBe("bicubic");
    expect(member?.request.scanScope.selection).toBe("all");
    expect(member?.request.concurrency).toBe(2);
    expect(result.plan.intentSnapshot.concurrency).toBe(2);
  });

  it("accepts only integer concurrency values from one through eight", () => {
    for (const concurrency of [1, 2, 4, 8]) {
      const result = planVerifyRunGroup({
        draft: { ...defaultVerifyDraft(), sourceIds: ["src_1"], concurrency },
        recipe: completeRecipe,
        sourcesById: { src_1: video },
      });
      expect(result.ok).toBe(true);
    }
    for (const concurrency of [0, 9, -1, 1.5, Number.NaN]) {
      const result = planVerifyRunGroup({
        draft: { ...defaultVerifyDraft(), sourceIds: ["src_1"], concurrency },
        recipe: completeRecipe,
        sourcesById: { src_1: video },
      });
      expect(result.ok).toBe(false);
      if (!result.ok) expect(result.reason).toBe("verify_concurrency_invalid");
    }
  });

  it("rejects incomplete recipes, missing sources, and non-video sources", () => {
    const draft = { ...defaultVerifyDraft(), sourceIds: ["src_1"] };
    const incomplete = planVerifyRunGroup({
      draft,
      recipe: { ...completeRecipe, kernel: null },
      sourcesById: { src_1: video },
    });
    expect(incomplete.ok).toBe(false);
    if (!incomplete.ok) expect(incomplete.reason).toBe("recipe_incomplete");

    const noSources = planVerifyRunGroup({
      draft: { ...draft, sourceIds: [] },
      recipe: completeRecipe,
      sourcesById: { src_1: video },
    });
    expect(noSources.ok).toBe(false);
    if (!noSources.ok) expect(noSources.reason).toBe("no_sources");

    const still = planVerifyRunGroup({
      draft,
      recipe: completeRecipe,
      sourcesById: { src_1: { ...video, kind: "still" } },
    });
    expect(still.ok).toBe(false);
    if (!still.ok) expect(still.reason).toBe("source_not_video");
  });
});

describe("extractVerifyFrames", () => {
  it("extracts real frame metrics and rejects fabricated shapes", () => {
    const rows = extractVerifyFrames({
      frames: [
        { frame: 10, metric: 0.02 },
        { frame: 11, metric: 0.03 },
      ],
    });
    expect(rows).toEqual([
      { frameIndex: 10, metric: 0.02 },
      { frameIndex: 11, metric: 0.03 },
    ]);
    expect(extractVerifyFrames(null)).toBeNull();
    expect(extractVerifyFrames({ frames: "nope" })).toBeNull();
    expect(extractVerifyFrames({ frames: [{ frame: "x", metric: 1 }] })).toBeNull();
  });
});
