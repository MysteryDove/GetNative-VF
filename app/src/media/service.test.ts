import { beforeEach, describe, expect, it, vi } from "vitest";

type Handler = (event: { payload: Record<string, unknown> }) => void;
const handlers = new Map<string, Set<Handler>>();
const order: string[] = [];
const invokeMock = vi.fn<(command: string, args?: unknown) => Promise<unknown>>();

vi.mock("@tauri-apps/api/core", () => ({
  invoke: (command: string, args?: unknown) => invokeMock(command, args),
}));

vi.mock("@tauri-apps/api/event", () => ({
  listen: (name: string, handler: Handler) => {
    let set = handlers.get(name);
    if (!set) {
      set = new Set();
      handlers.set(name, set);
    }
    set.add(handler);
    order.push(`listen:${name}`);
    return Promise.resolve(() => {
      set.delete(handler);
      order.push(`unlisten:${name}`);
    });
  },
}));

import { exportFrameAssetBatch, requestFrameWindow } from "./service";

function emit(name: string, payload: Record<string, unknown>): void {
  for (const handler of handlers.get(name) ?? []) {
    handler({ payload });
  }
}

function listenerCount(name: string): number {
  return handlers.get(name)?.size ?? 0;
}

describe("engine media service", () => {
  beforeEach(() => {
    handlers.clear();
    order.length = 0;
    invokeMock.mockReset();
  });

  it("listens before submitting one engine-owned frame asset batch", async () => {
    invokeMock.mockImplementation(async (command, args) => {
      order.push(command);
      if (command === "engine_worker_media_begin") {
        expect(listenerCount("engine-worker-event")).toBeGreaterThan(0);
        const request = (args as { request: { request_id: string } }).request;
        emit("engine-worker-event", { type: "accepted", request_id: request.request_id, job_id: "job-1" });
        emit("engine-worker-event", {
          type: "result",
          request_id: request.request_id,
          job_id: "job-1",
          payload: {
            decoded_frames: 3,
            assets: [{
              item_id: "item-1", frame_index: 7, path: "/cache/7.f32le",
              format: "f32le", width: 320, height: 240, from_cache: false,
            }],
          },
        });
      }
      return undefined;
    });

    await exportFrameAssetBatch({
      path: "/video.mkv",
      fingerprint: "fp",
      streamIndex: 0,
      width: 320,
      height: 240,
      frames: [{ itemId: "item-1", frameIndex: 7 }],
    }, async (asset) => {
      expect(asset.frameIndex).toBe(7);
      order.push("worker-submit");
    });

    expect(order.indexOf("listen:engine-worker-event")).toBeLessThan(
      order.indexOf("engine_worker_media_begin"),
    );
    expect(order).toContain("worker-submit");
    expect(order).not.toContain("media_frame_batch_ack");
  });

  it("cancels an accepted stale media request by engine job id", async () => {
    invokeMock.mockImplementation(async (command, args) => {
      order.push(command);
      if (command === "engine_worker_media_begin") {
        const request = (args as { request: { request_id: string } }).request;
        emit("engine-worker-event", { type: "accepted", request_id: request.request_id, job_id: "job-stale" });
      }
      return undefined;
    });
    const task = requestFrameWindow({
      path: "/video.mkv", streamIndex: 0, target: "frame", frameIndex: 9,
    });
    await vi.waitFor(() => expect(order).toContain("engine_worker_media_begin"));
    await task.cancel();
    expect(invokeMock).toHaveBeenCalledWith("engine_worker_cancel", { jobId: "job-stale" });
  });

  it("rejects a pending media task when the engine worker exits", async () => {
    invokeMock.mockImplementation(async (command, args) => {
      order.push(command);
      if (command === "engine_worker_media_begin") {
        const request = (args as { request: { request_id: string } }).request;
        emit("engine-worker-event", { type: "accepted", request_id: request.request_id, job_id: "job-x" });
      }
      return undefined;
    });
    const task = requestFrameWindow({
      path: "/video.mkv", streamIndex: 0, target: "frame", frameIndex: 9,
    });
    const settled = expect(task.promise).rejects.toThrow(/^worker_exit: /);
    await vi.waitFor(() => expect(order).toContain("engine_worker_media_begin"));
    emit("engine-worker-exit", { stderr_tail: ["segfault in decoder"] });
    await settled;
    expect(listenerCount("engine-worker-event")).toBe(0);
    expect(listenerCount("engine-worker-exit")).toBe(0);
  });

  it("settles every concurrent pending media task when the engine worker exits", async () => {
    invokeMock.mockImplementation(async (command, args) => {
      order.push(command);
      if (command === "engine_worker_media_begin") {
        const request = (args as { request: { request_id: string } }).request;
        emit("engine-worker-event", { type: "accepted", request_id: request.request_id, job_id: `job-${request.request_id}` });
      }
      return undefined;
    });
    const taskA = requestFrameWindow({
      path: "/a.mkv", streamIndex: 0, target: "frame", frameIndex: 1,
    });
    const taskB = requestFrameWindow({
      path: "/b.mkv", streamIndex: 0, target: "frame", frameIndex: 2,
    });
    const settledA = expect(taskA.promise).rejects.toThrow(
      "worker_exit: the engine worker exited before finishing the job",
    );
    const settledB = expect(taskB.promise).rejects.toThrow(
      "worker_exit: the engine worker exited before finishing the job",
    );
    await vi.waitFor(() => expect(
      order.filter((entry) => entry === "engine_worker_media_begin"),
    ).toHaveLength(2));
    expect(listenerCount("engine-worker-exit")).toBe(2);
    emit("engine-worker-exit", {});
    await Promise.all([settledA, settledB]);
    expect(listenerCount("engine-worker-event")).toBe(0);
    expect(listenerCount("engine-worker-exit")).toBe(0);
  });

  it("removes both listeners after normal completion", async () => {
    invokeMock.mockImplementation(async (command, args) => {
      order.push(command);
      if (command === "engine_worker_media_begin") {
        const request = (args as { request: { request_id: string } }).request;
        emit("engine-worker-event", { type: "accepted", request_id: request.request_id, job_id: "job-ok" });
        emit("engine-worker-event", {
          type: "result",
          request_id: request.request_id,
          job_id: "job-ok",
          payload: { frames: [] },
        });
      }
      return undefined;
    });
    const task = requestFrameWindow({
      path: "/video.mkv", streamIndex: 0, target: "frame", frameIndex: 9,
    });
    await expect(task.promise).resolves.toEqual({ frames: [] });
    expect(order).toContain("listen:engine-worker-exit");
    expect(order).toContain("unlisten:engine-worker-event");
    expect(order).toContain("unlisten:engine-worker-exit");
    expect(listenerCount("engine-worker-event")).toBe(0);
    expect(listenerCount("engine-worker-exit")).toBe(0);
  });
});
