import { beforeEach, describe, expect, it, vi } from "vitest";
import type { WorkerEvent } from "./protocol";

type WireEvent = Record<string, unknown>;
type EventHandler = (event: { payload: WireEvent }) => void;

const invokeMock = vi.fn<(command: string, args?: unknown) => Promise<unknown>>();
const eventHandlers = new Map<string, EventHandler>();

vi.mock("@tauri-apps/api/core", () => ({
  invoke: (command: string, args?: unknown) => invokeMock(command, args),
}));

vi.mock("@tauri-apps/api/event", () => ({
  listen: (name: string, handler: EventHandler) => {
    eventHandlers.set(name, handler);
    return Promise.resolve(() => eventHandlers.delete(name));
  },
}));

import { EngineWorkerClient } from "./workerClient";

function emitWire(payload: WireEvent) {
  eventHandlers.get("engine-worker-event")?.({ payload });
}

function emitExit(stderrTail: string[] = []) {
  eventHandlers.get("engine-worker-exit")?.({ payload: { stderr_tail: stderrTail } });
}

function collect(client: EngineWorkerClient): WorkerEvent[] {
  const events: WorkerEvent[] = [];
  client.subscribe((event) => events.push(event));
  return events;
}

async function connectedClient(): Promise<EngineWorkerClient> {
  invokeMock.mockImplementation((command: string) => {
    if (command === "engine_worker_start") {
      return Promise.resolve({
        path: "/engine",
        payload: { engine_version: "0.1.0", commands: { analyze: true, cancel: true } },
      });
    }
    return Promise.resolve({});
  });
  const client = new EngineWorkerClient();
  await client.connect();
  return client;
}

async function submitOne(
  client: EngineWorkerClient,
  onPrepared?: Parameters<EngineWorkerClient["submitHeight"]>[1],
) {
  const submitted = await client.submitHeight({
    frameAsset: { path: "/tmp/frame.f32le", format: "f32le", width: 320, height: 240 },
    axisMode: "h_only",
    kernel: { id: "bicubic", b: 0, c: 0.5 },
    candidates: ["230", "231"],
    metric: { cropLeft: 10, threshold: 0.015, pNorm: 1 },
    profileId: "muf-d278cd3",
    endpointRule: "exclusive_stop",
    baseHeight: "240",
    baseWidth: "320",
    grid: { start: "230", stop: "232", step: "1" },
  }, onPrepared);
  return submitted;
}

describe("EngineWorkerClient", () => {
  beforeEach(() => {
    invokeMock.mockReset();
    eventHandlers.clear();
  });

  it("connects, negotiates, and forwards the analyze command in wire shape", async () => {
    const client = await connectedClient();
    const submitted = await submitOne(client);
    expect(submitted.requestId).toBe("gui-req-1");

    const analyze = invokeMock.mock.calls.find(([command]) => command === "engine_worker_analyze");
    expect(analyze).toBeDefined();
    const request = (analyze?.[1] as { request: Record<string, unknown> }).request;
    expect(request).toMatchObject({
      requestId: "gui-req-1",
      mode: "height",
      frameAsset: { path: "/tmp/frame.f32le", format: "f32le", width: 320, height: 240 },
      axisMode: "h_only",
      kernel: { id: "bicubic", b: 0, c: 0.5 },
      candidates: ["230", "231"],
      backend: "cpu",
      profileId: "muf-d278cd3",
      endpointRule: "exclusive_stop",
      baseHeight: "240",
      baseWidth: "320",
      grid: { start: "230", stop: "232", step: "1" },
    });
  });

  it("translates wire events onto GUI job identity", async () => {
    const client = await connectedClient();
    const events = collect(client);
    const submitted = await submitOne(client);

    emitWire({
      protocol_version: 1,
      type: "accepted",
      request_id: submitted.requestId,
      job_id: "job-1",
      mode: "height",
      backend: "cuda",
      device: "NVIDIA GeForce RTX 5080",
      concurrency: 4,
      timestamp_ms: 10,
    });
    emitWire({
      protocol_version: 1,
      type: "progress",
      request_id: submitted.requestId,
      job_id: "job-1",
      completed: 5,
      total: 21,
      phase: "candidates",
      coverage: {
        selection: "every_n",
        eligible_frames: 100,
        selected_frames: 21,
        processed_frames: 5,
        failed_frames: 0,
      },
      timestamp_ms: 11,
    });
    emitWire({
      protocol_version: 1,
      type: "result",
      request_id: submitted.requestId,
      job_id: "job-1",
      mode: "height",
      payload: { candidates: [{ id: "230", error: 12.5 }] },
      coverage: {
        selection: "every_n",
        eligible_frames: 100,
        selected_frames: 21,
        processed_frames: 21,
        failed_frames: 0,
      },
      timestamp_ms: 12,
    });

    expect(events.map((event) => event.type)).toEqual(["accepted", "progress", "result"]);
    for (const event of events) {
      expect(event.jobId).toBe(submitted.jobId);
      expect(event.runId).toBe(submitted.runId);
      expect(event.requestId).toBe(submitted.requestId);
    }
    const progress = events[1];
    expect(progress.type === "progress" && progress.completed).toBe(5);
    expect(progress.type === "progress" && progress.detail).toBe("candidates");
    expect(progress.type === "progress" && progress.coverage).toEqual({
      selection: "every_n",
      eligibleFrames: 100,
      selectedFrames: 21,
      processedFrames: 5,
      failedFrames: 0,
    });
    const result = events[2];
    const accepted = events[0];
    expect(accepted.type === "accepted" && accepted.backend).toBe("cuda");
    expect(accepted.type === "accepted" && accepted.device).toBe("NVIDIA GeForce RTX 5080");
    expect(accepted.type === "accepted" && accepted.concurrency).toBe(4);
    expect(
      result.type === "result" &&
        (result.payload as { candidates: unknown[] }).candidates.length,
    ).toBe(1);
    expect(result.type === "result" && result.coverage?.processedFrames).toBe(21);
  });

  it("prepares local identity before invoke and survives accepted/result before invoke resolves", async () => {
    const client = await connectedClient();
    const events = collect(client);
    let resolveInvoke!: (value: unknown) => void;
    invokeMock.mockImplementation((command: string) => {
      if (command === "engine_worker_analyze") {
        return new Promise((resolve) => {
          resolveInvoke = resolve;
        });
      }
      return Promise.resolve({});
    });

    const prepared: Array<{ requestId: string; jobId: string; runId: string }> = [];
    const pending = submitOne(client, (job) => prepared.push(job));
    expect(prepared).toEqual([
      { requestId: "gui-req-1", jobId: "gui-job-1", runId: "gui-run-1" },
    ]);

    emitWire({
      type: "accepted",
      request_id: "gui-req-1",
      job_id: "engine-job-1",
      mode: "height",
      worker_count: 8,
      suggested_in_flight: 16,
    });
    emitWire({
      type: "result",
      request_id: "gui-req-1",
      job_id: "engine-job-1",
      mode: "height",
      payload: { candidates: [] },
    });
    resolveInvoke({ queued: true });
    await expect(pending).resolves.toEqual(prepared[0]);
    expect(events.map((event) => event.type)).toEqual(["accepted", "result"]);

    // Terminal cleanup removes both indexes: a stale engine-id event no longer
    // acquires the completed GUI identity.
    emitWire({ type: "progress", job_id: "engine-job-1", completed: 1, total: 1 });
    const stale = events.at(-1);
    expect(stale?.type).toBe("progress");
    if (stale?.type === "progress") expect(stale.jobId).toBe("engine-job-1");
  });

  it("correlates events that only carry the engine job id", async () => {
    const client = await connectedClient();
    const events = collect(client);
    const submitted = await submitOne(client);
    emitWire({
      type: "accepted",
      request_id: submitted.requestId,
      job_id: "job-7",
      mode: "height",
      timestamp_ms: 1,
    });
    emitWire({
      type: "cancelled",
      job_id: "job-7",
      partial: true,
      timestamp_ms: 2,
      coverage: {
        selection: "decoded_i_picture",
        eligible_frames: 240,
        selected_frames: 10,
        processed_frames: 3,
        failed_frames: 1,
      },
    });

    const cancelled = events[events.length - 1];
    expect(cancelled?.type).toBe("cancelled");
    if (cancelled?.type === "cancelled") {
      expect(cancelled.jobId).toBe(submitted.jobId);
      expect(cancelled.runId).toBe(submitted.runId);
      expect(cancelled.partial).toBe(true);
      expect(cancelled.coverage).toMatchObject({
        eligibleFrames: 240,
        processedFrames: 3,
        failedFrames: 1,
      });
    }
  });

  it("routes cancellation through the engine job id, even when cancel precedes accepted", async () => {
    const client = await connectedClient();
    const submitted = await submitOne(client);

    // Cancel before the engine job id is known: no invoke yet.
    await client.cancel(submitted.jobId);
    expect(
      invokeMock.mock.calls.filter(([command]) => command === "engine_worker_cancel"),
    ).toHaveLength(0);

    emitWire({
      type: "accepted",
      request_id: submitted.requestId,
      job_id: "job-3",
      mode: "height",
      timestamp_ms: 1,
    });
    // The deferred cancel is fired on accepted (fire-and-forget invoke).
    await vi.waitFor(() => {
      const cancels = invokeMock.mock.calls.filter(
        ([command]) => command === "engine_worker_cancel",
      );
      expect(cancels).toHaveLength(1);
      expect(cancels[0][1]).toEqual({ jobId: "job-3" });
    });

    // A second cancel after acceptance goes straight through.
    await client.cancel(submitted.jobId);
    const cancels = invokeMock.mock.calls.filter(
      ([command]) => command === "engine_worker_cancel",
    );
    expect(cancels).toHaveLength(2);
  });

  it("fails unfinished jobs when the worker exits and notifies exit listeners", async () => {
    const client = await connectedClient();
    const events = collect(client);
    const exits: string[][] = [];
    client.onExit((tail) => exits.push(tail));

    const first = await submitOne(client);
    emitWire({
      type: "result",
      request_id: first.requestId,
      job_id: "job-1",
      mode: "height",
      payload: {},
      timestamp_ms: 1,
    });
    const second = await client.submitHeight({
      frameAsset: { path: "/tmp/other.f32le", format: "f32le", width: 320, height: 240 },
      axisMode: "h_only",
      kernel: { id: "lanczos", taps: 3 },
      candidates: ["230"],
      metric: {},
    });

    emitExit(["cuda init failed"]);

    const synthesized = events.filter(
      (event) => event.type === "error" && event.code === "worker_exit",
    );
    expect(synthesized).toHaveLength(1);
    const failure = synthesized[0];
    if (failure.type === "error") {
      expect(failure.jobId).toBe(second.jobId);
      expect(failure.message).toBe("cuda init failed");
      expect(failure.retryable).toBe(false);
    }
    expect(exits).toEqual([["cuda init failed"]]);
  });

  it("emits a synthetic terminal failure with prepared ids and then drops tracking", async () => {
    const client = await connectedClient();
    const events = collect(client);
    invokeMock.mockImplementation((command: string) =>
      command === "engine_worker_analyze"
        ? Promise.reject(new Error("worker_not_running"))
        : Promise.resolve({}),
    );
    const prepared: Array<{ requestId: string; jobId: string; runId: string }> = [];
    await expect(
      client.submitHeight({
        frameAsset: { path: "/tmp/frame.f32le", format: "f32le", width: 320, height: 240 },
        axisMode: "h_only",
        kernel: { id: "bilinear" },
        candidates: ["230"],
        metric: {},
      }, (job) => prepared.push(job)),
    ).rejects.toThrow("worker_not_running");
    expect(prepared).toHaveLength(1);
    const synthetic = events.find((event) => event.type === "error");
    expect(synthetic).toMatchObject({
      type: "error",
      code: "submit_failed",
      requestId: "gui-req-1",
      jobId: "gui-job-1",
      runId: "gui-run-1",
    });

    // A late event for the rejected request id must not resolve to a job.
    emitWire({ type: "progress", request_id: "gui-req-1", completed: 1, total: 2 });
    const progress = events.at(-1);
    expect(progress).toBeDefined();
    if (progress?.type === "progress") {
      expect(progress.jobId).toBe("");
      expect(progress.runId).toBe("");
    }
  });

  it("adds finite frontend queue telemetry without mutating the wire payload", async () => {
    const client = await connectedClient();
    const events = collect(client);
    const submitted = await submitOne(client);
    const payload = {
      mode: "height",
      candidates: [],
      telemetry: { plan_ms: 1.25 },
    };
    emitWire({
      type: "accepted",
      request_id: submitted.requestId,
      job_id: "engine-job-telemetry",
      mode: "height",
    });
    emitWire({
      type: "result",
      request_id: submitted.requestId,
      job_id: "engine-job-telemetry",
      mode: "height",
      payload,
    });

    const result = events.find((event) => event.type === "result");
    expect(result?.type).toBe("result");
    if (result?.type === "result") {
      const telemetry = (result.payload as { telemetry: Record<string, number> }).telemetry;
      expect(Number.isFinite(telemetry.frontend_queue_ms)).toBe(true);
      expect(telemetry.plan_ms).toBe(1.25);
    }
    expect(payload).toEqual({
      mode: "height",
      candidates: [],
      telemetry: { plan_ms: 1.25 },
    });
  });
});
