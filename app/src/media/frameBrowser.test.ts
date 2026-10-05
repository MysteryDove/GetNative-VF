import { describe, expect, it } from "vitest";
import {
  findDuplicateSampleId,
  frameStepFromKeyboard,
  resolveFrameInput,
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
    assert(
      frameStepFromKeyboard("ArrowLeft", { shiftKey: true })?.type === "previousKeyframe",
      "shift+left",
    );
    assert(
      frameStepFromKeyboard("ArrowRight", { shiftKey: true })?.type === "nextKeyframe",
      "shift+right",
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

describe("resolveFrameInput", () => {
  it("clamps a typed frame number to the stream and rounds decimals", () => {
    expect(resolveFrameInput("1200", 34000)).toBe(1200);
    expect(resolveFrameInput(" 99999 ", 34000)).toBe(34000);
    expect(resolveFrameInput("12.6", 100)).toBe(13);
    expect(resolveFrameInput("0", 100)).toBe(0);
  });

  it("rejects anything that is not a non-negative number", () => {
    for (const text of ["", "abc", "-5", "1e3", "12a"]) {
      expect(resolveFrameInput(text, 100)).toBeNull();
    }
  });
});
