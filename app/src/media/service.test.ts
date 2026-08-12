import { beforeEach, describe, expect, it, vi } from "vitest";

type Handler = (event: { payload: Record<string, unknown> }) => void;
const handlers = new Map<string, Handler>();
const order: string[] = [];
const invokeMock = vi.fn<(command: string, args?: unknown) => Promise<unknown>>();

vi.mock("@tauri-apps/api/core", () => ({
  invoke: (command: string, args?: unknown) => invokeMock(command, args),
}));

vi.mock("@tauri-apps/api/event", () => ({
  listen: (name: string, handler: Handler) => {
    handlers.set(name, handler);
    order.push(`listen:${name}`);
    return Promise.resolve(() => handlers.delete(name));
  },
}));

import { exportFrameAssetBatch, requestFrameWindow } from "./service";

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
        expect(handlers.has("engine-worker-event")).toBe(true);
        const request = (args as { request: { request_id: string } }).request;
        handlers.get("engine-worker-event")?.({
          payload: { type: "accepted", request_id: request.request_id, job_id: "job-1" },
        });
        handlers.get("engine-worker-event")?.({
          payload: {
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
        handlers.get("engine-worker-event")?.({
          payload: { type: "accepted", request_id: request.request_id, job_id: "job-stale" },
        });
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
});
