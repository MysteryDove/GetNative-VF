import { describe, expect, it } from "vitest";
import type { ProjectState, Run, RunGroup } from "../project/types";
import { emptyProjectState } from "../project/normalize";
import {
  removeRunFromState,
  removeRunGroupFromState,
  runActualBackend,
  runDecodeProvenance,
  sourceFilterLabel,
} from "./ResultsPage";

function runWithTelemetry(telemetry: Record<string, unknown>): Run {
  return { result: { telemetry } } as Run;
}

describe("runActualBackend", () => {
  it("reads the worker result telemetry without changing the project schema", () => {
    expect(runActualBackend(runWithTelemetry({
      backend: "cuda",
      cuda_device: "NVIDIA GeForce RTX 5080",
    }))).toEqual({ backend: "cuda", device: "NVIDIA GeForce RTX 5080" });
    expect(runActualBackend(runWithTelemetry({
      backend: "vulkan",
      vulkan_device: "Discrete Vulkan GPU",
    }))).toEqual({ backend: "vulkan", device: "Discrete Vulkan GPU" });
    expect(runActualBackend(runWithTelemetry({
      backend: "cpu",
      vulkan_device: "stale value",
    }))).toEqual({ backend: "cpu", device: undefined });
  });

  it("keeps older or malformed result payloads compatible", () => {
    expect(runActualBackend({ result: null } as Run)).toBeNull();
    expect(runActualBackend(runWithTelemetry({ backend: "auto" }))).toBeNull();
  });
});

describe("runDecodeProvenance", () => {
  it("reads zero-copy state and the final fallback reason", () => {
    const run = {
      result: {
        provenance: {
          decoder: "software",
          zero_copy: false,
          fallback_chain: [
            { reason: "unsupported profile" },
            { reason: "decoder failed at frame 12" },
          ],
        },
      },
    } as Run;
    expect(runDecodeProvenance(run)).toEqual({
      decoder: "software",
      zeroCopy: false,
      fallbackReason: "decoder failed at frame 12",
    });
  });

  it("keeps legacy results compatible", () => {
    expect(runDecodeProvenance({ result: { telemetry: {} } } as Run)).toBeNull();
  });
});

describe("sourceFilterLabel", () => {
  it("uses the source label or filename and adds a short fingerprint", () => {
    const state = {
      sourcesById: {
        first: { id: "first", path: "C:/media/SourceA.mkv", label: "SourceA", fingerprint: "123456789", videoStreams: [] },
        second: { id: "second", path: "C:/media/123.png", fingerprint: null, videoStreams: [] },
      },
    } as unknown as ProjectState;
    expect(sourceFilterLabel("first", state)).toBe("SourceA (#123456)");
    expect(sourceFilterLabel("second", state)).toBe("123.png");
    expect(sourceFilterLabel("missing", state)).toBe("missing");
  });

  it("omits the quick fingerprint algorithm prefix", () => {
    const state = {
      sourcesById: {
        source: {
          id: "source",
          path: "C:/media/00001.m2ts",
          fingerprint: "quick-sha256-v1:abcdef123456",
          videoStreams: [],
        },
      },
    } as unknown as ProjectState;
    expect(sourceFilterLabel("source", state)).toBe("00001.m2ts (#abcdef)");
  });
});

function deletionState(): ProjectState {
  const state = emptyProjectState();
  state.runsById = {
    first: {
      id: "first",
      status: "completed",
      runGroupId: "group",
    } as Run,
    second: {
      id: "second",
      status: "completed",
      runGroupId: "group",
    } as Run,
    unrelated: {
      id: "unrelated",
      status: "completed",
      runGroupId: null,
    } as Run,
  };
  state.runGroupsById = {
    group: {
      id: "group",
      memberRunIds: ["first", "second"],
    } as RunGroup,
  };
  state.verificationReviewsByRunId = {
    first: { runId: "first", tags: [] },
    second: { runId: "second", tags: [] },
  };
  return state;
}

describe("Results deletion state", () => {
  it("removes a RunGroup, its member Runs, and their reviews without mutating the input", () => {
    const state = deletionState();
    const next = removeRunGroupFromState(state, "group");

    expect(next).not.toBe(state);
    expect(next.runGroupsById.group).toBeUndefined();
    expect(next.runsById.first).toBeUndefined();
    expect(next.runsById.second).toBeUndefined();
    expect(next.runsById.unrelated).toBe(state.runsById.unrelated);
    expect(next.verificationReviewsByRunId).toEqual({});
    expect(state.runGroupsById.group).toBeDefined();
    expect(state.runsById.first).toBeDefined();
  });

  it("removes one Run from its group and removes the group with its final member", () => {
    const state = deletionState();
    const withoutFirst = removeRunFromState(state, "first");
    expect(withoutFirst.runGroupsById.group.memberRunIds).toEqual(["second"]);
    expect(withoutFirst.runsById.first).toBeUndefined();

    const withoutSecond = removeRunFromState(withoutFirst, "second");
    expect(withoutSecond.runGroupsById.group).toBeUndefined();
    expect(withoutSecond.runsById.unrelated).toBeDefined();
  });

  it("does not delete queued or running data", () => {
    const state = deletionState();
    state.runsById.first = { ...state.runsById.first, status: "running" };

    expect(removeRunFromState(state, "first")).toBe(state);
    expect(removeRunGroupFromState(state, "group")).toBe(state);
  });
});
