import { describe, expect, it } from "vitest";
import { startVerifyRunGroup, type VerifyFrameEntry } from "./executeVerify";
import { planVerifyRunGroup, defaultVerifyDraft } from "./verifyPlan";
import { emptyProjectState } from "../project/normalize";
import type { ProjectState, Recipe, Source } from "../project/types";
import type { QueueJobInput } from "./runReducer";
import type { WorkerEvent } from "./protocol";

const recipe: Recipe = {
  id: "recipe_1",
  name: "Bicubic 720p",
  status: "locked",
  locked: true,
  revision: 1,
  parentRecipeId: null,
  createdAt: "2026-08-01T00:00:00Z",
  updatedAt: "2026-08-01T00:00:00Z",
  geometry: {
    mode: "standard",
    activeWidth: 1920,
    activeHeight: 1080,
    canvasWidth: 1280,
    canvasHeight: 720,
    srcLeft: 0,
    srcTop: 0,
    srcWidth: 1920,
    srcHeight: 1080,
    baseWidth: null,
    baseHeight: 720,
    parity: null,
  },
  kernel: { id: "bicubic", parameters: { b: 0, c: 0.5 } },
  metric: {
    cropLeft: 10,
    cropRight: 10,
    cropTop: 10,
    cropBottom: 10,
    pixelExclusionThreshold: 0.015,
    pNorm: 1,
  },
  axisMode: "h_only",
  profileId: "muf-d278cd3",
  mathMode: "raw",
};

const video: Source = {
  id: "src_1",
  kind: "video",
  path: "/tmp/episode.mkv",
  fingerprint: "fp-1",
  state: "ready",
  videoStreams: [],
  selectedStreamIndex: 0,
  width: 1920,
  height: 1080,
};

type Listener = (event: WorkerEvent) => void;

function makeFakeWorker(
  frameErrors: Array<number | null>,
  options: {
    coverage?: boolean;
    eligibleFrames?: number;
    selection?: "all" | "decoded_i_picture" | "every_n";
    reverse?: boolean;
    terminal?: "result" | "cancelled" | "error";
  } = {},
) {
  const listeners = new Set<Listener>();
  const worker = {
    subscribe(listener: Listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    async verifyMediaBegin(params: unknown, onPrepared?: (job: { requestId: string; jobId: string; runId: string }) => void) {
      mediaParams.push(params);
      const job = { requestId: "req-1", jobId: "gui-job-1", runId: "gui-run-1" };
      onPrepared?.(job);
      queueMicrotask(() => {
        let results = frameErrors.map((error, seq) => ({
          seq,
          frameIndex: seq * 2,
          pts: seq * 1001,
          timestampSeconds: seq / 24,
          error,
        }));
        if (options.reverse) results = [...results].reverse();
        const processedFrames = frameErrors.filter((error) => error !== null).length;
        const failedFrames = frameErrors.length - processedFrames;
        const coverage = {
          selection: options.selection ?? "decoded_i_picture" as const,
          eligibleFrames: options.eligibleFrames ?? frameErrors.length,
          selectedFrames: frameErrors.length,
          processedFrames,
          failedFrames,
        };
        for (const listener of listeners) {
          listener({
            protocolVersion: 1, requestId: "req-1", jobId: "gui-job-1",
            runId: "gui-run-1", timestampMs: 1, type: "progress",
            completed: results.length, total: results.length, results,
            ...(options.coverage === false ? {} : { coverage }),
          });
          if (options.terminal === "cancelled") {
            listener({
              protocolVersion: 1, requestId: "req-1", jobId: "gui-job-1",
              runId: "gui-run-1", timestampMs: 2, type: "cancelled", partial: true,
              ...(options.coverage === false ? {} : { coverage }),
            });
          } else if (options.terminal === "error") {
            listener({
              protocolVersion: 1, requestId: "req-1", jobId: "gui-job-1",
              runId: "gui-run-1", timestampMs: 2, type: "error",
              code: "media_decode_error", message: "decode failed", retryable: false,
            });
          } else {
            listener({
              protocolVersion: 1, requestId: "req-1", jobId: "gui-job-1",
              runId: "gui-run-1", timestampMs: 2, type: "result", mode: "verify",
              payload: {
                mode: "verify",
                frames_completed: processedFrames,
                frames_failed: failedFrames,
              },
              ...(options.coverage === false ? {} : { coverage }),
            });
          }
        }
      });
      return job;
    },
    async waitAccepted() {
      return { jobId: "engine-job-1", workerCount: 4, suggestedInFlight: 8 };
    },
    async cancel() {},
  };
  const mediaParams: unknown[] = [];
  return { worker, mediaParams };
}

describe("startVerifyRunGroup", () => {
  it("drives the full stream and merges frames into the stored result", async () => {
    const draft = { ...defaultVerifyDraft(), sourceIds: ["src_1"] };
    const planned = planVerifyRunGroup({
      draft,
      recipe,
      sourcesById: { src_1: video },
      nowMs: 1,
      requestIdPrefix: "t",
    });
    expect(planned.ok).toBe(true);
    if (!planned.ok) return;

    const { worker, mediaParams } = makeFakeWorker([0.5, 1e-7, null]);
    const liveBatches: VerifyFrameEntry[] = [];
    let state: ProjectState = emptyProjectState({ id: "p1" });
    state.sourcesById.src_1 = video;
    const queued: QueueJobInput[] = [];

    const result = await startVerifyRunGroup({
      plan: planned.plan,
      recipe,
      state,
      onProjectChange: (updater) => {
        state = updater(state);
      },
      bridge: { queue: (input) => queued.push(input), cancel: () => {} },
      onFrames: (_runId, entries) => liveBatches.push(...entries),
      deps: {
        worker,
        nowMs: () => 1000,
      },
    });

    expect(result.ok).toBe(true);
    const run = Object.values(state.runsById)[0];
    expect(run?.runType).toBe("verification");
    expect(run?.status).toBe("completed");
    // Streamed frames merged into the stored result, errors kept verbatim
    // (including the null for the failed frame) with indexed identities.
    const stored = run?.result as {
      frames?: Array<{ seq: number; error: number | null }>;
      coverage?: { eligibleFrames: number; processedFrames: number; failedFrames: number };
    };
    expect(stored.frames?.map((frame) => frame.error)).toEqual([0.5, 1e-7, null]);
    expect(stored.coverage).toMatchObject({
      eligibleFrames: 3,
      processedFrames: 2,
      failedFrames: 1,
    });
    expect(liveBatches).toHaveLength(3);
    expect(liveBatches.map((frame) => frame.frameIndex)).toEqual([0, 2, 4]);
    expect(queued[0]?.mode).toBe("verify");
    expect(queued[0]?.total).toBe(0);
    expect(mediaParams).toMatchObject([{ concurrency: 2 }]);
    expect((run?.inputSnapshot as { concurrency?: number }).concurrency).toBe(2);
  });

  it("retains and sorts all 34,072 frame metrics for the terminal write", async () => {
    const draft = { ...defaultVerifyDraft(), sourceIds: ["src_1"], scopeKind: "full" as const };
    const planned = planVerifyRunGroup({
      draft,
      recipe,
      sourcesById: { src_1: video },
      nowMs: 1,
      requestIdPrefix: "t",
    });
    expect(planned.ok).toBe(true);
    if (!planned.ok) return;

    const frameCount = 34_072;
    const { worker } = makeFakeWorker(
      Array.from({ length: frameCount }, (_, index) => index / frameCount),
      { selection: "all", eligibleFrames: frameCount, reverse: true },
    );
    let state: ProjectState = emptyProjectState({ id: "p1" });
    state.sourcesById.src_1 = video;

    const result = await startVerifyRunGroup({
      plan: planned.plan,
      recipe,
      state,
      onProjectChange: (updater) => {
        state = updater(state);
      },
      bridge: { queue: () => {}, cancel: () => {} },
      deps: { worker, nowMs: () => 1000 },
    });

    expect(result.ok).toBe(true);
    const run = Object.values(state.runsById)[0];
    expect(run?.status).toBe("completed");
    expect(run?.completed).toBe(frameCount);
    const stored = run?.result as { frames?: Array<{ seq: number; frameIndex: number }> };
    expect(stored.frames).toHaveLength(frameCount);
    expect(stored.frames?.[0]).toMatchObject({ seq: 0, frameIndex: 0 });
    expect(stored.frames?.[stored.frames.length - 1]?.seq).toBe(frameCount - 1);
    expect(stored.frames?.[stored.frames.length - 1]?.frameIndex).toBe((frameCount - 1) * 2);
  });

  it("persists cancelled coverage and notifies the live buffer after the terminal write", async () => {
    const draft = { ...defaultVerifyDraft(), sourceIds: ["src_1"] };
    const planned = planVerifyRunGroup({
      draft,
      recipe,
      sourcesById: { src_1: video },
      nowMs: 1,
      requestIdPrefix: "t",
    });
    expect(planned.ok).toBe(true);
    if (!planned.ok) return;

    const { worker } = makeFakeWorker([0.5, 0.25], {
      terminal: "cancelled",
      eligibleFrames: 100,
    });
    let state: ProjectState = emptyProjectState({ id: "p1" });
    state.sourcesById.src_1 = video;
    const terminalRuns: string[] = [];
    await startVerifyRunGroup({
      plan: planned.plan,
      recipe,
      state,
      onProjectChange: (updater) => { state = updater(state); },
      bridge: { queue: () => {}, cancel: () => {} },
      onTerminal: (runId) => terminalRuns.push(runId),
      deps: { worker, nowMs: () => 1000 },
    });

    const run = Object.values(state.runsById)[0];
    expect(run?.status).toBe("partial");
    expect(run?.result).toMatchObject({
      coverage: { eligibleFrames: 100, selectedFrames: 2, processedFrames: 2 },
    });
    expect(terminalRuns).toEqual([run?.id]);
  });

  it("keeps terminal results compatible when an older worker omits coverage", async () => {
    const draft = { ...defaultVerifyDraft(), sourceIds: ["src_1"] };
    const planned = planVerifyRunGroup({
      draft, recipe, sourcesById: { src_1: video }, nowMs: 1, requestIdPrefix: "t",
    });
    expect(planned.ok).toBe(true);
    if (!planned.ok) return;
    const { worker } = makeFakeWorker([0.5], { coverage: false });
    let state: ProjectState = emptyProjectState({ id: "p1" });
    state.sourcesById.src_1 = video;
    await startVerifyRunGroup({
      plan: planned.plan,
      recipe,
      state,
      onProjectChange: (updater) => { state = updater(state); },
      bridge: { queue: () => {}, cancel: () => {} },
      deps: { worker, nowMs: () => 1000 },
    });
    expect(Object.values(state.runsById)[0]?.result).not.toHaveProperty("coverage");
  });

  it("keeps every streamed metric when a worker fails after partial progress", async () => {
    const draft = { ...defaultVerifyDraft(), sourceIds: ["src_1"] };
    const planned = planVerifyRunGroup({
      draft, recipe, sourcesById: { src_1: video }, nowMs: 1, requestIdPrefix: "t",
    });
    expect(planned.ok).toBe(true);
    if (!planned.ok) return;
    const { worker } = makeFakeWorker([0.2, 0.1], {
      terminal: "error", eligibleFrames: 100, reverse: true,
    });
    let state: ProjectState = emptyProjectState({ id: "p1" });
    state.sourcesById.src_1 = video;
    await startVerifyRunGroup({
      plan: planned.plan,
      recipe,
      state,
      onProjectChange: (updater) => { state = updater(state); },
      bridge: { queue: () => {}, cancel: () => {} },
      deps: { worker, nowMs: () => 1000 },
    });
    const run = Object.values(state.runsById)[0];
    expect(run?.status).toBe("failed");
    expect((run?.result as { frames: VerifyFrameEntry[] }).frames.map((frame) => frame.seq))
      .toEqual([0, 1]);
    expect(run?.result).toMatchObject({ coverage: { eligibleFrames: 100 } });
  });
});
