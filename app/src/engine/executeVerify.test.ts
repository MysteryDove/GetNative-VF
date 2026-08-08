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

function makeFakeWorker(frameErrors: Array<number | null>) {
  const listeners = new Set<Listener>();
  const sentFrames: number[] = [];
  const worker = {
    subscribe(listener: Listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    async verifyBegin() {
      return { requestId: "req-1", jobId: "gui-job-1", runId: "gui-run-1" };
    },
    async waitAccepted() {
      return "engine-job-1";
    },
    async verifyFrame(input: { seq: number }) {
      sentFrames.push(input.seq);
      return undefined;
    },
    async verifyEnd() {
      // On end: emit batched results then the terminal result.
      const results = frameErrors.map((error, seq) => ({ seq, error }));
      for (const listener of listeners) {
        listener({
          protocolVersion: 1,
          requestId: "req-1",
          jobId: "gui-job-1",
          runId: "gui-run-1",
          timestampMs: 1,
          type: "progress",
          completed: results.length,
          total: results.length,
          results,
        });
        listener({
          protocolVersion: 1,
          requestId: "req-1",
          jobId: "gui-job-1",
          runId: "gui-run-1",
          timestampMs: 2,
          type: "result",
          mode: "verify",
          payload: { mode: "verify", frames_completed: results.length, frames_failed: 0 },
        });
      }
    },
  };
  return { worker, sentFrames };
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

    const { worker } = makeFakeWorker([0.5, 1e-7, null]);
    const acked: number[] = [];
    const liveBatches: VerifyFrameEntry[] = [];
    let state: ProjectState = emptyProjectState({ id: "p1" });
    state.sourcesById.src_1 = video;
    const queued: QueueJobInput[] = [];

    const assetListeners: Array<(event: {
      ticket: string;
      seq: number;
      frameIndex: number;
      path: string;
      width: number;
      height: number;
    }) => void> = [];

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
        streamStart: async () => ({ ticket: "tk", total: 3, width: 1920, height: 1080 }),
        streamAck: async (_ticket, seq) => {
          acked.push(seq);
        },
        streamAbort: async () => {},
        listenAsset: async (handler) => {
          assetListeners.push(handler);
          return () => {};
        },
        listenDone: async (handler) => {
          // Emit done after assets: schedule on microtask after listeners set.
          queueMicrotask(() => {});
          setTimeout(() => handler({ ticket: "tk", produced: 3 }), 0);
          return () => {};
        },
        listenError: async () => () => {},
        sleep: (ms) => new Promise((resolve) => setTimeout(resolve, ms)),
        nowMs: () => 1000,
      },
      // Emit the three assets once listeners are wired (next tick).
    });

    expect(result.ok).toBe(true);
    const run = Object.values(state.runsById)[0];
    expect(run?.runType).toBe("verification");
    expect(run?.status).toBe("completed");
    // Streamed frames merged into the stored result, errors kept verbatim
    // (including the null for the failed frame), and assets acknowledged.
    const stored = run?.result as { frames?: Array<{ seq: number; error: number | null }> };
    expect(stored.frames?.map((frame) => frame.error)).toEqual([0.5, 1e-7, null]);
    expect(acked.sort()).toEqual([0, 1, 2]);
    expect(liveBatches).toHaveLength(3);
    expect(queued[0]?.mode).toBe("verify");
    expect(queued[0]?.total).toBe(3);
  });
});
