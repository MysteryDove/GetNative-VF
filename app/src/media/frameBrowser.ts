/** Pure helpers for Media frame-browser interaction contracts (GUI-2). */

export type FrameStepAction =
  | { type: "previousFrame" }
  | { type: "nextFrame" }
  | { type: "previousKeyframe" }
  | { type: "nextKeyframe" }
  | { type: "firstFrame" }
  | { type: "lastFrame" };

/**
 * Keyboard map for the silent frame browser.
 * Arrow keys step frames; [ ] step keyframes; Home/End jump ends.
 * Inputs that are currently focused should not use this helper.
 */
export function frameStepFromKeyboard(
  key: string,
  options?: { shiftKey?: boolean },
): FrameStepAction | null {
  switch (key) {
    case "ArrowLeft":
    case "j":
    case "J":
      return { type: "previousFrame" };
    case "ArrowRight":
    case "k":
    case "K":
      return { type: "nextFrame" };
    case "[":
      return { type: "previousKeyframe" };
    case "]":
      return { type: "nextKeyframe" };
    case "Home":
      return { type: "firstFrame" };
    case "End":
      return { type: "lastFrame" };
    case "ArrowUp":
      return options?.shiftKey ? { type: "previousKeyframe" } : null;
    case "ArrowDown":
      return options?.shiftKey ? { type: "nextKeyframe" } : null;
    default:
      return null;
  }
}

export type SampleIdentity = {
  sourceId: string;
  streamIndex?: number | null;
  frameIndex?: number | null;
  kind: "still" | "video" | "animated";
};

/** True when an indistinguishable still/frame Sample already exists. */
export function findDuplicateSampleId(
  samples: Array<{
    id: string;
    sourceId: string;
    streamIndex?: number | null;
    frameIndex?: number | null;
  }>,
  candidate: SampleIdentity,
): string | null {
  if (candidate.kind === "animated") return null;
  for (const sample of samples) {
    if (sample.sourceId !== candidate.sourceId) continue;
    if (candidate.kind === "still") {
      if (sample.frameIndex == null && sample.streamIndex == null) return sample.id;
      continue;
    }
    if (
      sample.streamIndex === candidate.streamIndex &&
      sample.frameIndex === candidate.frameIndex
    ) {
      return sample.id;
    }
  }
  return null;
}
