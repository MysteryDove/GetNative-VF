/**
 * Engine worker transport client (worker protocol v1).
 *
 * Owns the GUI side of the resident `getnative-engine worker` session:
 * lifecycle commands go through Tauri invokes, job events arrive as
 * `engine-worker-event` Tauri events in wire form (snake_case, per
 * `docs/worker-protocol-v1.md`) and are translated here onto the camelCase
 * semantic events of `protocol.ts`.
 *
 * Identity model: the GUI assigns requestId/jobId/runId when a job is
 * submitted; the engine assigns its own `job_id` in the `accepted` event.
 * This module keeps both mappings so cancellation and event correlation
 * never depend on engine naming.
 */

import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";
import type { EngineEnvelope } from "./types";
import type {
  AxisMode,
  ComputationMode,
  JobId,
  RequestId,
  RunId,
  WorkerEvent,
} from "./protocol";

export type FrameAssetRef = {
  path: string;
  format: "f32le";
  width: number;
  height: number;
};

export type HeightJobParams = {
  frameAsset: FrameAssetRef;
  axisMode: AxisMode;
  kernel: { id: string; b?: number; c?: number; taps?: number };
  candidates: string[];
  metric: {
    cropLeft?: number;
    cropRight?: number;
    cropTop?: number;
    cropBottom?: number;
    threshold?: number;
    pNorm?: number;
  };
  backend?: "cpu" | "cuda" | "auto";
  workerCount?: number;
};

export type KernelJobParams = {
  frameAsset: FrameAssetRef;
  axisMode: AxisMode;
  /** Fixed primary-axis value (decimal string); wire candidates = [candidate]. */
  candidate: string;
  /** Ordered kernel list; result rows key on the decimal index into this list. */
  kernels: Array<{ id: string; b?: number; c?: number; taps?: number }>;
  metric: HeightJobParams["metric"];
  backend?: "cpu" | "cuda" | "auto";
  workerCount?: number;
};

export type WorkerHello = {
  path: string;
  payload: {
    engine_version: string;
    commands: { analyze: boolean; cancel: boolean };
  };
};

export type SubmittedJob = {
  requestId: RequestId;
  jobId: JobId;
  runId: RunId;
};

type WireEvent = {
  protocol_version?: number;
  type?: string;
  request_id?: string;
  job_id?: string;
  timestamp_ms?: number;
  mode?: string;
  phase?: string;
  completed?: number;
  total?: number;
  code?: string;
  message?: string;
  retryable?: boolean;
  partial?: boolean;
  payload?: unknown;
};

type TrackedJob = {
  requestId: RequestId;
  jobId: JobId;
  runId: RunId;
  engineJobId: string | null;
  finished: boolean;
  cancelRequested: boolean;
};

type WorkerEventListener = (event: WorkerEvent) => void;
type WorkerExitListener = (stderrTail: string[]) => void;

export class EngineWorkerClient {
  private sequence = 0;
  private jobsByRequest = new Map<RequestId, TrackedJob>();
  private jobsByEngineId = new Map<string, TrackedJob>();
  private eventListeners = new Set<WorkerEventListener>();
  private exitListeners = new Set<WorkerExitListener>();
  private unlistenEvent?: UnlistenFn;
  private unlistenExit?: UnlistenFn;
  private hello: WorkerHello | null = null;
  private connectPromise: Promise<WorkerHello> | null = null;

  /** Spawn (or attach to) the worker and subscribe to its event streams.
   * Idempotent: concurrent callers share one connection attempt. */
  connect(): Promise<WorkerHello> {
    this.connectPromise ??= this.connectInner().catch((error: unknown) => {
      this.connectPromise = null;
      throw error;
    });
    return this.connectPromise;
  }

  private async connectInner(): Promise<WorkerHello> {
    if (this.unlistenEvent === undefined) {
      this.unlistenEvent = await listen<WireEvent>("engine-worker-event", (event) => {
        this.handleWireEvent(event.payload);
      });
    }
    if (this.unlistenExit === undefined) {
      this.unlistenExit = await listen<{ stderr_tail?: string[] }>(
        "engine-worker-exit",
        (event) => this.handleExit(event.payload?.stderr_tail ?? []),
      );
    }
    const hello = await invoke<WorkerHello>("engine_worker_start");
    this.hello = hello;
    return hello;
  }

  /** Worker-session capabilities; reports commands.analyze per the backend contract. */
  async capabilities(): Promise<EngineEnvelope> {
    return invoke<EngineEnvelope>("engine_worker_capabilities");
  }

  async shutdown(): Promise<void> {
    await invoke("engine_worker_shutdown");
    this.hello = null;
    this.connectPromise = null;
  }

  get negotiated(): WorkerHello | null {
    return this.hello;
  }

  subscribe(listener: WorkerEventListener): () => void {
    this.eventListeners.add(listener);
    return () => this.eventListeners.delete(listener);
  }

  onExit(listener: WorkerExitListener): () => void {
    this.exitListeners.add(listener);
    return () => this.exitListeners.delete(listener);
  }

  /** Submit one height-mode analyze job. Events stream to subscribers. */
  async submitHeight(params: HeightJobParams): Promise<SubmittedJob> {
    return this.submitAnalyze("height", {
      kernel: params.kernel,
      candidates: params.candidates,
      frameAsset: params.frameAsset,
      axisMode: params.axisMode,
      metric: params.metric,
      backend: params.backend ?? "cpu",
      workerCount: params.workerCount,
    });
  }

  /** Submit one kernel-mode analyze job (protocol v1.1): fixed geometry, ordered kernels. */
  async submitKernel(params: KernelJobParams): Promise<SubmittedJob> {
    return this.submitAnalyze("kernel", {
      kernels: params.kernels,
      candidates: [params.candidate],
      frameAsset: params.frameAsset,
      axisMode: params.axisMode,
      metric: params.metric,
      backend: params.backend ?? "cpu",
      workerCount: params.workerCount,
    });
  }

  private async submitAnalyze(
    mode: "height" | "kernel",
    body: Record<string, unknown>,
  ): Promise<SubmittedJob> {
    this.sequence += 1;
    const submitted: SubmittedJob = {
      requestId: `gui-req-${this.sequence}`,
      jobId: `gui-job-${this.sequence}`,
      runId: `gui-run-${this.sequence}`,
    };
    this.jobsByRequest.set(submitted.requestId, {
      ...submitted,
      engineJobId: null,
      finished: false,
      cancelRequested: false,
    });
    try {
      await invoke("engine_worker_analyze", {
        request: {
          requestId: submitted.requestId,
          mode,
          ...body,
        },
      });
    } catch (error) {
      this.jobsByRequest.delete(submitted.requestId);
      throw error;
    }
    return submitted;
  }

  /** Cooperative cancel. Safe to call before the engine job id is known. */
  async cancel(jobId: JobId): Promise<void> {
    const tracked = [...this.jobsByRequest.values()].find((job) => job.jobId === jobId);
    if (!tracked || tracked.finished) return;
    tracked.cancelRequested = true;
    if (tracked.engineJobId !== null) {
      await invoke("engine_worker_cancel", { jobId: tracked.engineJobId });
    }
    // Otherwise the cancel is sent as soon as `accepted` arrives.
  }

  private emit(event: WorkerEvent): void {
    for (const listener of this.eventListeners) {
      listener(event);
    }
  }

  private resolveTracked(wire: WireEvent): TrackedJob | undefined {
    if (wire.request_id !== undefined) {
      const tracked = this.jobsByRequest.get(wire.request_id);
      if (tracked) return tracked;
    }
    if (wire.job_id !== undefined) {
      return this.jobsByEngineId.get(wire.job_id);
    }
    return undefined;
  }

  private handleWireEvent(wire: WireEvent): void {
    if (typeof wire.type !== "string") return;
    if (wire.type === "shutdown") {
      this.handleExit([]);
      return;
    }

    const tracked = this.resolveTracked(wire);
    if (wire.type === "accepted" && tracked && typeof wire.job_id === "string") {
      tracked.engineJobId = wire.job_id;
      this.jobsByEngineId.set(wire.job_id, tracked);
      if (tracked.cancelRequested) {
        void invoke("engine_worker_cancel", { jobId: wire.job_id }).catch(() => undefined);
      }
    }
    if (
      tracked &&
      (wire.type === "result" || wire.type === "cancelled" || wire.type === "error")
    ) {
      tracked.finished = true;
    }

    const base = {
      protocolVersion: 1 as const,
      requestId: wire.request_id ?? tracked?.requestId ?? "",
      jobId: tracked?.jobId ?? wire.job_id ?? "",
      runId: tracked?.runId ?? "",
      timestampMs: wire.timestamp_ms ?? 0,
    };

    switch (wire.type) {
      case "accepted":
        this.emit({ ...base, type: "accepted", mode: wireMode(wire.mode) });
        break;
      case "progress":
        this.emit({
          ...base,
          type: "progress",
          completed: wire.completed ?? 0,
          total: wire.total ?? 0,
          detail: wire.phase ?? null,
        });
        break;
      case "warning":
        this.emit({
          ...base,
          type: "warning",
          code: wire.code ?? "internal",
          message: wire.message ?? "",
        });
        break;
      case "result":
        this.emit({
          ...base,
          type: "result",
          mode: wireMode(wire.mode),
          payload: wire.payload,
        });
        break;
      case "cancelled":
        this.emit({ ...base, type: "cancelled", partial: wire.partial ?? false });
        break;
      case "error":
        this.emit({
          ...base,
          type: "error",
          code: wire.code ?? "internal",
          message: wire.message ?? "",
          retryable: wire.retryable ?? false,
        });
        break;
      default:
        break;
    }
  }

  private handleExit(stderrTail: string[]): void {
    // A dead worker never finishes its jobs: fail every unfinished one so the
    // Job Tray cannot show them as running forever.
    const message =
      stderrTail.length > 0
        ? stderrTail.join("\n")
        : "the engine worker exited before finishing the job";
    for (const tracked of this.jobsByRequest.values()) {
      if (tracked.finished) continue;
      tracked.finished = true;
      this.emit({
        protocolVersion: 1,
        requestId: tracked.requestId,
        jobId: tracked.jobId,
        runId: tracked.runId,
        timestampMs: Date.now(),
        type: "error",
        code: "worker_exit",
        message,
        retryable: false,
      });
    }
    this.jobsByRequest.clear();
    this.jobsByEngineId.clear();
    this.hello = null;
    this.connectPromise = null;
    for (const listener of this.exitListeners) {
      listener(stderrTail);
    }
  }
}

function wireMode(mode: string | undefined): ComputationMode {
  return mode === "kernel" || mode === "verify" ? mode : "height";
}

/** Process-wide session used by the app shell and (later) the analysis pages. */
export const engineWorker = new EngineWorkerClient();
