import { describe, expect, it } from "vitest";
import { createTranslator } from "../i18n";
import { emptyProjectState } from "../project/normalize";
import type { Run } from "../project/types";
import {
  reviewVerifyFrames,
  verificationRunLabel,
  verifyCoverageDisplay,
  worstVerifyFrameInRange,
} from "./verifyResults";

const t = createTranslator("en");

function makeRun(overrides: Partial<Run> = {}): Run {
  return {
    id: "run-1",
    runType: "verification",
    status: "completed",
    runGroupId: null,
    sampleId: null,
    sourceId: "source-1",
    createdAt: "2026-08-18T00:00:00Z",
    updatedAt: "2026-08-18T00:00:00Z",
    inputSnapshot: null,
    result: null,
    errorCode: null,
    errorMessage: null,
    completed: 0,
    total: 0,
    ...overrides,
  };
}

describe("verification result presentation", () => {
  it("distinguishes same-source runs by range and nested request recipe", () => {
    const state = emptyProjectState({ id: "project-1" });
    state.sourcesById["source-1"] = {
      id: "source-1", kind: "video", path: "/media/episode.mkv", label: "Episode 01",
      state: "ready", videoStreams: [],
    };
    state.recipesById["recipe-full"] = { id: "recipe-full", name: "Full Recipe" } as never;
    state.recipesById["recipe-i"] = { id: "recipe-i", name: "I-frame Recipe" } as never;
    const full = makeRun({
      id: "full",
      inputSnapshot: { request: { recipeId: "recipe-full" }, scanScope: { selection: "all" } },
      completed: 34_072,
      result: {
        coverage: {
          selection: "all", eligibleFrames: 34_072, selectedFrames: 34_072,
          processedFrames: 34_072, failedFrames: 0,
        },
      },
    });
    const preview = makeRun({
      id: "preview",
      inputSnapshot: {
        request: { recipeId: "recipe-i" },
        scanScope: { selection: "decoded_i_picture" },
      },
      completed: 1_609,
      result: {
        coverage: {
          selection: "decoded_i_picture", eligibleFrames: 34_072, selectedFrames: 1_609,
          processedFrames: 1_609, failedFrames: 0,
        },
      },
    });

    expect(verificationRunLabel(full, state, t)).toBe(
      "Episode 01 · Full Verification · Full Recipe",
    );
    expect(verificationRunLabel(preview, state, t)).toBe(
      "Episode 01 · Decoded I-pictures only · I-frame Recipe",
    );
    expect(verifyCoverageDisplay(full).text).toBe("34072/34072");
    expect(verifyCoverageDisplay(preview)).toMatchObject({
      text: "1609/34072",
      badge: "incomplete",
    });
  });

  it("labels every-N custom ranges and supports legacy top-level recipeId", () => {
    const state = emptyProjectState({ id: "project-1" });
    state.sourcesById["source-1"] = {
      id: "source-1", kind: "video", path: "clip.mkv", state: "ready", videoStreams: [],
    };
    state.recipesById["recipe-old"] = { id: "recipe-old", name: "Legacy Recipe" } as never;
    const run = makeRun({
      inputSnapshot: {
        recipeId: "recipe-old",
        scanScope: { selection: "every_n", everyN: 24, startFrame: 100, endFrame: 500 },
      },
    });
    expect(verificationRunLabel(run, state, t)).toBe(
      "clip.mkv · Every 24 frames (#100 to #500) · Legacy Recipe",
    );
  });

  it("does not treat an old preview selected total as eligible coverage", () => {
    const oldPreview = makeRun({
      inputSnapshot: { scanScope: { selection: "every_n", everyN: 24 } },
      completed: 1_609,
      total: 1_609,
      result: { frames: [{ seq: 0, frameIndex: 0, error: 0.1 }] },
    });
    expect(verifyCoverageDisplay(oldPreview)).toMatchObject({
      text: "1609/?",
      badge: "incomplete",
      metricsIncomplete: true,
    });
  });

  it("detects historical truncated metrics without fabricating missing frames", () => {
    const truncated = makeRun({
      inputSnapshot: { scanScope: { selection: "all" } },
      completed: 10,
      total: 10,
      result: {
        frames: [
          { seq: 8, frameIndex: 8, error: 0.1 },
          { seq: 9, frameIndex: 9, error: 0.2 },
        ],
      },
    });
    expect(verifyCoverageDisplay(truncated).metricsIncomplete).toBe(true);
  });

  it("keeps early high-error frames in threshold, Top N, and zoom-range review", () => {
    const frames = [
      { seq: 0, frameIndex: 0, error: 99 },
      { seq: 1, frameIndex: 100, error: 0.01 },
      { seq: 2, frameIndex: 34_071, error: 5 },
    ];
    expect(reviewVerifyFrames(frames, 1, 2).map((frame) => frame.frameIndex)).toEqual([0, 34_071]);
    expect(worstVerifyFrameInRange(frames, { xMin: 0, xMax: 100 })?.frameIndex).toBe(0);
  });
});
