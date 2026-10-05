import { describe, expect, it } from "vitest";
import type { ProjectState } from "../project/types";
import { filterHeightSeriesRunIds } from "./HeightResultsPanel";

describe("filterHeightSeriesRunIds", () => {
  const state = {
    runsById: {
      run1: { id: "run1", runGroupId: "group1", sourceId: "sourceA" },
      run2: { id: "run2", runGroupId: "group1", sourceId: "sourceB" },
      run3: { id: "run3", runGroupId: "group2", sourceId: "sourceA" },
      run4: { id: "run4", runGroupId: null, sourceId: null },
    },
    samplesById: {
      sample4: { id: "sample4", sourceId: "sourceC" },
    },
  } as unknown as ProjectState;
  const series = [
    { runId: "run1", sampleId: "sample1" },
    { runId: "run2", sampleId: "sample2" },
    { runId: "run3", sampleId: "sample3" },
    { runId: "run4", sampleId: "sample4" },
  ];

  it("filters RunGroups rather than their member run ids", () => {
    expect([...filterHeightSeriesRunIds(series, state, ["group1"], [])]).toEqual([
      "run1",
      "run2",
    ]);
  });

  it("intersects the RunGroup and Source selections", () => {
    expect([...filterHeightSeriesRunIds(series, state, ["group1"], ["sourceA"])]).toEqual([
      "run1",
    ]);
    expect([...filterHeightSeriesRunIds(series, state, ["group2"], ["sourceB"])]).toEqual([]);
  });

  it("keeps legacy ungrouped runs addressable and falls back to the sample source", () => {
    expect([...filterHeightSeriesRunIds(series, state, ["run:run4"], ["sourceC"])]).toEqual([
      "run4",
    ]);
  });

  it("unions several selected Runs and several selected Sources", () => {
    expect([...filterHeightSeriesRunIds(series, state, ["group2", "run:run4"], [])]).toEqual([
      "run3",
      "run4",
    ]);
    expect([...filterHeightSeriesRunIds(series, state, [], ["sourceB", "sourceC"])]).toEqual([
      "run2",
      "run4",
    ]);
    expect([...filterHeightSeriesRunIds(series, state, ["group1", "group2"], ["sourceA"])]).toEqual([
      "run1",
      "run3",
    ]);
  });

  it("narrows to the selected Samples (frames)", () => {
    expect([...filterHeightSeriesRunIds(series, state, [], [], ["sample2", "sample3"])]).toEqual([
      "run2",
      "run3",
    ]);
    expect([...filterHeightSeriesRunIds(series, state, ["group1"], [], ["sample3"])]).toEqual([]);
  });
});
