import { describe, expect, it } from "vitest";
import {
  applyTerminalEventToRun,
  kernelParamsForWire,
  startHeightRunGroup,
  type ExecutionBridge,
} from "./executeRunGroup";
import { planHeightRunGroup } from "./runGroupPlan";
import { extractHeightSeries } from "./runGroupPlan";
import { defaultHeightDraft } from "./heightDraft";
import { emptyProjectState } from "../project/normalize";
import type { ProjectState } from "../project/types";
import type { QueueJobInput } from "./runReducer";
import type { HeightJobParams } from "./workerClient";

const samples = [
  {
    id: "smp_1",
    label: "frame 12",
    sourceId: "src_1",
    sourceFingerprint: "fp-a",
    streamIndex: 0,
    frameIndex: 12,
    included: true,
  },
  {
    id: "smp_2",
    label: "frame 40",
    sourceId: "src_1",
    sourceFingerprint: "fp-a",
    streamIndex: 0,
    frameIndex: 40,
    included: true,
  },
];

const sources = {
  src_1: {
    id: "src_1",
    path: "/tmp/clip.mkv",
    fingerprint: "fp-a",
    state: "ready",
    width: 1920,
    height: 1080,
  },
};

function makePlan() {
  const draft = {
    ...defaultHeightDraft(null),
    start: "710",
    stop: "712",
    step: "1",
  };
  const result = planHeightRunGroup({
    draft,
    samples,
    sourcesById: sources,
    capabilities: null,
    nowMs: 1,
    requestIdPrefix: "test",
  });
  if (!result.ok) throw new Error(`plan failed: ${result.reason}`);
  return result.plan;
}

function makeBridge() {
  const queued: QueueJobInput[] = [];
  const bridge: ExecutionBridge = {
    queue(input) {
      queued.push(input);
    },
    cancel() {},
  };
  return { bridge, queued };
}

describe("kernelParamsForWire", () => {
  it("maps only relevant numeric parameters per kernel", () => {
    expect(kernelParamsForWire({ id: "bicubic", parameters: { b: "0", c: "0.5" } })).toEqual({
      id: "bicubic",
      b: 0,
      c: 0.5,
    });
    expect(kernelParamsForWire({ id: "lanczos", parameters: { taps: "3" } })).toEqual({
      id: "lanczos",
      taps: 3,
    });
    expect(kernelParamsForWire({ id: "spline36", parameters: {} })).toEqual({ id: "spline36" });
  });
});

describe("startHeightRunGroup", () => {
  it("materializes the group, exports assets, submits members, and binds runs", async () => {
    const plan = makePlan();
    let state: ProjectState = emptyProjectState({ id: "p1" });
    const { bridge, queued } = makeBridge();
    const submittedParams: HeightJobParams[] = [];

    const result = await startHeightRunGroup({
      plan,
      state,
      onProjectChange: (updater) => {
        state = updater(state);
      },
      bridge,
      exportAsset: async () => ({
        path: "/cache/frame.f32",
        format: "f32le",
        width: 1920,
        height: 1080,
        from_cache: false,
      }),
      worker: {
        submitHeight: async (params) => {
          submittedParams.push(params);
          return {
            requestId: `req-${submittedParams.length}`,
            jobId: `job-${submittedParams.length}`,
            runId: `gui-run-${submittedParams.length}`,
          };
        },
      },
      nowMs: () => 1000,
    });

    expect(result).toEqual({ ok: true, runGroupId: expect.any(String), submitted: 2, failed: 0 });

    // Project records: one group, two queued runs with immutable snapshots.
    const groups = Object.values(state.runGroupsById);
    expect(groups).toHaveLength(1);
    const runs = Object.values(state.runsById);
    expect(runs).toHaveLength(2);
    expect(runs.every((run) => run.status === "queued" && run.result === null)).toBe(true);

    // Wire params: grid candidates, metric threshold mapping, bicubic numbers.
    expect(submittedParams).toHaveLength(2);
    expect(submittedParams[0]?.candidates).toEqual(["710", "711", "712"]);
    expect(submittedParams[0]?.metric.threshold).toBe(0);
    expect(submittedParams[0]?.metric.pNorm).toBe(1);
    expect(submittedParams[0]?.kernel.id).toBe("bicubic");
    expect(submittedParams[0]?.frameAsset).toEqual({
      path: "/cache/frame.f32",
      format: "f32le",
      width: 1920,
      height: 1080,
    });

    // Live jobs are bound to their persistent Project Run ids.
    expect(queued).toHaveLength(2);
    expect(queued[0]?.projectRunId).toBe(runs[0]?.id);
    expect(queued[0]?.total).toBe(3);
    expect(queued[0]?.runGroupId).toBe(groups[0]?.id);
  });

  it("marks a member failed when its frame asset fails and continues the rest", async () => {
    const plan = makePlan();
    let state: ProjectState = emptyProjectState({ id: "p1" });
    const { bridge } = makeBridge();
    let call = 0;

    const result = await startHeightRunGroup({
      plan,
      state,
      onProjectChange: (updater) => {
        state = updater(state);
      },
      bridge,
      exportAsset: async () => {
        call += 1;
        if (call === 1) throw new Error("frame_asset_error: decode failed");
        return {
          path: "/cache/frame.f32",
          format: "f32le",
          width: 1920,
          height: 1080,
          from_cache: false,
        };
      },
      worker: {
        submitHeight: async () => ({ requestId: "r", jobId: "j", runId: "g" }),
      },
      nowMs: () => 1000,
    });

    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.submitted).toBe(1);
      expect(result.failed).toBe(1);
    }
    const statuses = Object.values(state.runsById)
      .map((run) => run.status)
      .sort();
    expect(statuses).toEqual(["failed", "queued"]);
    const failedRun = Object.values(state.runsById).find((run) => run.status === "failed");
    expect(failedRun?.errorCode).toBe("frame_asset_error");
  });
});

describe("applyTerminalEventToRun", () => {
  function stateWithQueuedRun(): { state: ProjectState; runId: string } {
    const state = emptyProjectState({ id: "p1" });
    state.runsById.run_1 = {
      id: "run_1",
      runType: "height",
      status: "running",
      runGroupId: null,
      sampleId: "smp_1",
      sourceId: "src_1",
      createdAt: "2026-08-08T00:00:00Z",
      updatedAt: "2026-08-08T00:00:00Z",
      inputSnapshot: null,
      result: null,
      errorCode: null,
      errorMessage: null,
      completed: 1,
      total: 3,
    };
    return { state, runId: "run_1" };
  }

  it("applies results append-only and never rewrites terminal runs", () => {
    const { state, runId } = stateWithQueuedRun();
    const payload = {
      mode: "height",
      candidates: [
        { id: "710", error: 12.5 },
        { id: "711", error: 3.25 },
      ],
      telemetry: { plan_cache_hits: 1 },
    };
    const completed = applyTerminalEventToRun(
      state,
      runId,
      { type: "result", payload },
      "2026-08-08T00:00:05Z",
    );
    const run = completed.runsById[runId];
    expect(run?.status).toBe("completed");
    expect(run?.completed).toBe(3);
    expect(run?.result).toEqual(payload);
    // The stored payload feeds the AnalyzePage series extractor as-is.
    expect(extractHeightSeries(run?.result)).toEqual([
      { height: "710", metric: 12.5 },
      { height: "711", metric: 3.25 },
    ]);

    const rewritten = applyTerminalEventToRun(
      completed,
      runId,
      { type: "error", code: "internal", message: "late failure" },
      "2026-08-08T00:00:06Z",
    );
    expect(rewritten).toBe(completed);
  });

  it("records failures and partial cancellations honestly", () => {
    const { state, runId } = stateWithQueuedRun();
    const failed = applyTerminalEventToRun(
      state,
      runId,
      { type: "error", code: "unsupported", message: "p_norm" },
      "2026-08-08T00:00:05Z",
    );
    expect(failed.runsById[runId]?.status).toBe("failed");
    expect(failed.runsById[runId]?.errorCode).toBe("unsupported");

    const { state: state2, runId: run2 } = stateWithQueuedRun();
    const partial = applyTerminalEventToRun(
      state2,
      run2,
      { type: "cancelled", partial: true, payload: { candidates: [] } },
      "2026-08-08T00:00:05Z",
    );
    expect(partial.runsById[run2]?.status).toBe("partial");
  });
});

describe("startKernelRunGroup", () => {
  it("submits fixed-geometry ordered kernel lists and binds runs", async () => {
    const { startKernelRunGroup } = await import("./executeRunGroup");
    const { planKernelRunGroup } = await import("./kernelRunGroup");
    const { defaultKernelDraft } = await import("./kernelDraft");

    const draft = defaultKernelDraft(null, {
      cropLeft: 0, cropRight: 0, cropTop: 0, cropBottom: 0,
      pixelExclusionThreshold: 0, pNorm: 1,
    }, "muf-d278cd3", "raw", "auto");
    draft.families = {
      bilinear: true, bicubic: true, spline16: false,
      spline36: false, spline64: false, lanczos: false,
    };
    const geometry = {
      mode: "standard" as const,
      activeWidth: 1920, activeHeight: 1080,
      canvasWidth: 1280, canvasHeight: 720,
      srcLeft: 0, srcTop: 0, srcWidth: 1920, srcHeight: 1080,
      baseWidth: null, baseHeight: 720, parity: null,
    };
    const key = geometryGroupKeyForTest(draft);
    const planned = planKernelRunGroup({
      draft,
      samples: [samples[0]!],
      sourcesById: sources,
      geometries: { [key]: geometry },
      capabilities: null,
      nowMs: 1,
      requestIdPrefix: "t",
    });
    expect(planned.ok).toBe(true);
    if (!planned.ok) return;

    let state: ProjectState = emptyProjectState({ id: "p1" });
    const { bridge, queued } = makeBridge();
    const submissions: Array<Record<string, unknown>> = [];
    const result = await startKernelRunGroup({
      plan: planned.plan,
      state,
      onProjectChange: (updater) => { state = updater(state); },
      bridge,
      exportAsset: async () => ({
        path: "/cache/f.f32", format: "f32le", width: 1920, height: 1080, from_cache: false,
      }),
      worker: {
        submitKernel: async (params) => {
          submissions.push(params as unknown as Record<string, unknown>);
          return { requestId: "r1", jobId: "j1", runId: "g1" };
        },
      },
      nowMs: () => 1000,
    });

    expect(result.ok).toBe(true);
    expect(submissions).toHaveLength(1);
    expect(submissions[0]?.candidate).toBe("720");
    expect(submissions[0]?.kernels).toEqual([
      { id: "bilinear" },
      { id: "bicubic", b: 0, c: 0.5 },
    ]);
    expect(queued[0]?.mode).toBe("kernel");
    expect(queued[0]?.total).toBe(2);
    const run = Object.values(state.runsById)[0];
    expect(run?.runType).toBe("kernel");
    expect(run?.status).toBe("queued");
  });
});

function geometryGroupKeyForTest(draft: { baseHeight: string; baseWidth: string; profileId: string }) {
  // Same derivation the panel uses for the 1920x1080 fixture source.
  return ["1920", "1080", draft.baseHeight || "auto", draft.baseWidth || "auto", draft.profileId].join("@");
}
