/** Pure helpers for Media frame-browser interaction contracts (GUI-2). */

import type { FrameWindowTarget, MediaFrameWindow } from "./service";

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

/** Minimal keyboard-event shape the dispatch needs (React/DOM agnostic). */
export type FrameBrowserKeyEvent = {
  key: string;
  shiftKey: boolean;
  target: EventTarget | null;
  preventDefault: () => void;
};

/**
 * Keyboard dispatch for the frame browser and viewport: ignores keystrokes
 * from form fields and while a decode is in flight, then routes the mapped
 * FrameStepAction through `select` with clamping at the window bounds.
 */
export function dispatchFrameBrowserKey(
  event: FrameBrowserKeyEvent,
  deps: {
    frameWindow: MediaFrameWindow | null;
    previewBusy: boolean;
    select: (target: FrameWindowTarget, frameIndex?: number) => void;
  },
): void {
  const { frameWindow, previewBusy, select } = deps;
  if (!frameWindow || previewBusy) return;
  const target = event.target as HTMLElement | null;
  if (
    target &&
    (target.tagName === "INPUT" ||
      target.tagName === "SELECT" ||
      target.tagName === "TEXTAREA" ||
      target.getAttribute("role") === "slider")
  ) {
    return;
  }
  const action = frameStepFromKeyboard(event.key, { shiftKey: event.shiftKey });
  if (!action) return;
  event.preventDefault();
  const current = frameWindow.selected.frame_index;
  const max = Math.max(0, (frameWindow.total_frames ?? 1) - 1);
  switch (action.type) {
    case "previousFrame":
      if (current > 0) select("frame", current - 1);
      break;
    case "nextFrame":
      if (current < max) select("frame", current + 1);
      break;
    case "previousKeyframe":
      if (frameWindow.previous_keyframe != null) select("previousKeyframe", current);
      break;
    case "nextKeyframe":
      if (frameWindow.next_keyframe != null) select("nextKeyframe", current);
      break;
    case "firstFrame":
      select("frame", 0);
      break;
    case "lastFrame":
      select("frame", max);
      break;
    default:
      break;
  }
}

/** True when a Sample already covers the source's given frame (stills: any sample). */
export function isSourceFrameSampled(
  samples: Array<{
    sourceId: string;
    streamIndex?: number | null;
    frameIndex?: number | null;
  }>,
  source: {
    id: string;
    kind: "still" | "video" | "animated";
    selectedStreamIndex?: number | null;
  },
  frameIndex: number,
): boolean {
  return samples.some(
    (sample) =>
      sample.sourceId === source.id &&
      (source.kind !== "video" ||
        (sample.streamIndex === source.selectedStreamIndex &&
          sample.frameIndex === frameIndex)),
  );
}

export type SampleIdentity = {  sourceId: string;
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
