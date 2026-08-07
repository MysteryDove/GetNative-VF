import { describe, expect, it } from "vitest";
import { emptyProjectState } from "./normalize";
import { buildRunExportCsv, buildRunExportJson, extractRunRows } from "./export";
import type { Run } from "./types";

function makeRun(overrides: Partial<Run>): Run {
  return {
    id: "run_1",
    runType: "height",
    status: "completed",
    runGroupId: null,
    sampleId: null,
    sourceId: null,
    createdAt: "2026-08-01T00:00:00Z",
    updatedAt: "2026-08-01T00:01:00Z",
    inputSnapshot: null,
    result: null,
    errorCode: null,
    errorMessage: null,
    completed: 3,
    total: 3,
    ...overrides,
  };
}

describe("export", () => {
  it("extracts height rows only from real results", () => {
    const run = makeRun({
      result: { series: [{ height: "720", metric: 0.012 }] },
    });
    expect(extractRunRows(run)).toEqual([
      { runId: "run_1", runType: "height", seriesKey: "720", metric: 0.012 },
    ]);
    expect(extractRunRows(makeRun({ result: null }))).toEqual([]);
  });

  it("builds CSV with a header and raw numerics only", () => {
    const state = emptyProjectState({ id: "p1", name: "Demo" });
    state.runsById.run_1 = makeRun({
      result: { series: [{ height: "720", metric: 0.012 }] },
    });
    state.runsById.run_2 = makeRun({ id: "run_2", result: null });
    const csv = buildRunExportCsv(["run_1", "run_2"], state);
    expect(csv).toBe("run_id,run_type,series_key,metric\nrun_1,height,720,0.012\n");
    expect(buildRunExportCsv(["run_2"], state)).toBeNull();
  });

  it("builds JSON with full provenance", () => {
    const state = emptyProjectState({ id: "p1", name: "Demo" });
    state.runGroupsById.rgrp_1 = {
      id: "rgrp_1",
      memberRunIds: ["run_1"],
      groupType: "single_height",
      label: "Resolution Test",
      createdAt: "2026-08-01T00:00:00Z",
      intentSnapshot: null,
    };
    state.runsById.run_1 = makeRun({ runGroupId: "rgrp_1" });
    const parsed = JSON.parse(buildRunExportJson(state, ["run_1"]));
    expect(parsed.kind).toBe("getnative_run_export");
    expect(parsed.project.id).toBe("p1");
    expect(parsed.run_groups).toHaveLength(1);
    expect(parsed.runs[0].run_group_id).toBe("rgrp_1");
  });
});
