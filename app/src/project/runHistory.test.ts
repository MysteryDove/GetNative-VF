import { describe, expect, it } from "vitest";
import {
  removeRunFamilyFromState,
  removeRunsFromState,
  runFamilyCounts,
  runSelectionRunIds,
} from "./runHistory";
import type { ProjectState, Run } from "./types";

function run(id: string, runType: string, groupId: string | null, status = "completed"): Run {
  return {
    id, runType, status, runGroupId: groupId, sampleId: null, sourceId: "s",
    createdAt: "2026-01-01T00:00:00Z", updatedAt: "2026-01-01T00:00:00Z",
    inputSnapshot: null, result: null, errorCode: null, errorMessage: null,
    completed: 0, total: 0,
  };
}

function state(): ProjectState {
  const runs = [
    run("h1", "height", "gh"), run("h2", "height", "gh"), run("h3", "height", "gh2", "running"),
    run("k1", "kernel", "gk"), run("v1", "verification", "gv"), run("v2", "verify", null),
  ];
  const group = (id: string, members: string[]) => ({
    id, memberRunIds: members, groupType: "t", label: id,
    createdAt: "2026-01-01T00:00:00Z", intentSnapshot: null,
  });
  return {
    runsById: Object.fromEntries(runs.map((item) => [item.id, item])),
    runGroupsById: {
      gh: group("gh", ["h1", "h2"]), gh2: group("gh2", ["h3"]),
      gk: group("gk", ["k1"]), gv: group("gv", ["v1"]),
    },
    verificationReviewsByRunId: { v1: { runId: "v1", tags: [] } },
    verificationFusionsById: { f: {} },
  } as unknown as ProjectState;
}

describe("run history", () => {
  it("counts runs per test, including the legacy verify type", () => {
    const counts = runFamilyCounts(state());
    expect(counts.height).toEqual({ total: 3, active: 1 });
    expect(counts.kernel).toEqual({ total: 1, active: 0 });
    expect(counts.verify).toEqual({ total: 2, active: 0 });
  });

  it("removes one test's finished runs and leaves active runs and other tests", () => {
    const next = removeRunFamilyFromState(state(), "height");
    expect(Object.keys(next.runsById).sort()).toEqual(["h3", "k1", "v1", "v2"]);
    expect(Object.keys(next.runGroupsById).sort()).toEqual(["gh2", "gk", "gv"]);
    expect(Object.keys(next.verificationFusionsById)).toEqual(["f"]);
  });

  it("drops reviews and saved Fusions with the last Check run", () => {
    const next = removeRunFamilyFromState(state(), "verify");
    expect(Object.keys(next.runsById).sort()).toEqual(["h1", "h2", "h3", "k1"]);
    expect(next.verificationReviewsByRunId).toEqual({});
    expect(next.verificationFusionsById).toEqual({});
  });

  it("shrinks a group when only some members are removed and resolves selector values", () => {
    const base = state();
    const next = removeRunsFromState(base, ["h1", "h3"]);
    expect(next.runGroupsById.gh.memberRunIds).toEqual(["h2"]);
    expect(next.runsById.h3).toBeDefined();
    expect(removeRunsFromState(base, ["missing"])).toBe(base);
    expect(runSelectionRunIds(base, "gh")).toEqual(["h1", "h2"]);
    expect(runSelectionRunIds(base, "run:v2")).toEqual(["v2"]);
  });
});
