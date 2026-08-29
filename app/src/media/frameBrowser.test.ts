import { describe, it } from "vitest";
import {
  findDuplicateSampleId,
  frameStepFromKeyboard,
} from "./frameBrowser";

function assert(condition: unknown, message: string): asserts condition {
  if (!condition) throw new Error(message);
}

describe("GUI-2 frame browser contracts", () => {
  it("maps keyboard shortcuts for frame and keyframe stepping", () => {
    assert(frameStepFromKeyboard("ArrowLeft")?.type === "previousFrame", "left");
    assert(frameStepFromKeyboard("ArrowRight")?.type === "nextFrame", "right");
    assert(frameStepFromKeyboard("[")?.type === "previousKeyframe", "[");
    assert(frameStepFromKeyboard("]")?.type === "nextKeyframe", "]");
    assert(frameStepFromKeyboard("Home")?.type === "firstFrame", "home");
    assert(frameStepFromKeyboard("End")?.type === "lastFrame", "end");
    assert(frameStepFromKeyboard("j")?.type === "previousFrame", "j");
    assert(frameStepFromKeyboard("k")?.type === "nextFrame", "k");
    assert(
      frameStepFromKeyboard("ArrowUp", { shiftKey: true })?.type === "previousKeyframe",
      "shift+up",
    );
    assert(frameStepFromKeyboard("a") === null, "unrelated key");
  });

  it("detects indistinguishable still and frame Sample duplicates", () => {
    const samples = [
      { id: "s1", sourceId: "src_still", streamIndex: null, frameIndex: null },
      { id: "s2", sourceId: "src_vid", streamIndex: 0, frameIndex: 12 },
    ];
    assert(
      findDuplicateSampleId(samples, {
        sourceId: "src_still",
        kind: "still",
      }) === "s1",
      "still duplicate",
    );
    assert(
      findDuplicateSampleId(samples, {
        sourceId: "src_vid",
        kind: "video",
        streamIndex: 0,
        frameIndex: 12,
      }) === "s2",
      "frame duplicate",
    );
    assert(
      findDuplicateSampleId(samples, {
        sourceId: "src_vid",
        kind: "video",
        streamIndex: 0,
        frameIndex: 13,
      }) === null,
      "new frame",
    );
  });
});
