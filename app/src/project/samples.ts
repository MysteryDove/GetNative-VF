import type { ProjectState, Sample } from "./types";

/**
 * Sample selectors and shared Source-error patches used by Media/Samples
 * pages. Pure transforms so they plug directly into `onProjectChange`.
 */

/** Samples with `included: true`, sorted by `order` (the analysis input set). */
export function includedSamples(state: ProjectState): Sample[] {
  return Object.values(state.samplesById)
    .filter((sample) => sample.included)
    .sort((a, b) => a.order - b.order);
}

/**
 * Returns a NEW ProjectState with the given Source patched to `state: "error"`
 * plus the provided errorCode/errorDetail. Missing source returns the input
 * state unchanged.
 */
export function applySourceError(
  state: ProjectState,
  sourceId: string,
  error: { code: string; detail: string },
): ProjectState {
  const source = state.sourcesById[sourceId];
  if (!source) return state;
  return {
    ...state,
    sourcesById: {
      ...state.sourcesById,
      [sourceId]: {
        ...source,
        state: "error",
        errorCode: error.code,
        errorDetail: error.detail,
      },
    },
  };
}
