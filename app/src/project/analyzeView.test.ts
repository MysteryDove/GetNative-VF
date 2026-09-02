import { describe, expect, it } from "vitest";
import { analyzeViewState, parseStoredMetric } from "./analyzeView";
import { emptyProjectState } from "./normalize";

describe("analyzeViewState", () => {
  it("defaults to collapsed with no stored metric", () => {
    expect(analyzeViewState(emptyProjectState())).toEqual({
      metricSpecOpen: false,
      metric: null,
    });
  });

  it("restores a saved MetricSpec and open flag", () => {
    const metric = {
      cropLeft: 5,
      cropRight: 5,
      cropTop: 4,
      cropBottom: 4,
      pixelExclusionThreshold: 0.015,
      pNorm: 2,
    };
    const state = emptyProjectState();
    state.uiStateByRoute.analyze = { metricSpecOpen: true, metric };
    expect(analyzeViewState(state)).toEqual({ metricSpecOpen: true, metric });
  });

  it("rejects incomplete stored metrics", () => {
    expect(parseStoredMetric({ cropLeft: 5, pNorm: 1 })).toBeNull();
  });
});
