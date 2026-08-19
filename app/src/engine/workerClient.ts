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
  ActualBackend,
  ComputationMode,
  JobId,
  RequestId,
  RunId,
  VerifyCoverage,
  WorkerEvent,
  EndpointRule,
  GeometryWire,
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
  backend?: "cpu" | "cuda" | "vulkan" | "auto";
  workerCount?: number;
  profileId?: string;
  endpointRule?: EndpointRule;
  baseHeight?: string | null;
  baseWidth?: string | null;
  grid?: { start: string; stop: string; step: string };
  geometry?: GeometryWire | null;
};

export type KernelJobParams = {
  frameAsset: FrameAssetRef;
  axisMode: AxisMode;
  /** Fixed primary-axis value (decimal string); wire candidates = [candidate]. */
  candidate: string;
  /** Ordered kernel list; result rows key on the decimal index into this list. */
  kernels: Array<{ id: string; b?: number; c?: number; taps?: number }>;
  metric: HeightJobParams["metric"];
  backend?: "cpu" | "cuda" | "vulkan" | "auto";
  workerCount?: number;
  profileId?: string;
  endpointRule?: EndpointRule;
  baseHeight?: string | null;
  baseWidth?: string | null;
  geometry?: GeometryWire | null;
};

export type VerifyMediaJobParams = {
  path: string;
  fingerprint?: string | null;
  streamIndex: number;
  width: number;
  height: number;
  selection: "all" | "decoded_i_picture" | "every_n";
  everyN?: number | null;
  startFrame?: number | null;
  endFrame?: number | null;
  axisMode: AxisMode;
  kernel: { id: string; b?: number; c?: number; taps?: number };
  candidate: string;
  metric: HeightJobParams["metric"];
  backend?: "cpu" | "cuda" | "vulkan" | "auto";
  concurrency: number;
  geometry?: GeometryWire | null;
};

export type WorkerHello = {
  path: string;
  payload: {
    engine_version: string;
    commands: {
      analyze: boolean;
      cancel: boolean;
      media_index_begin?: boolean;
      media_frame_window?: boolean;
      media_preview_begin?: boolean;
      media_asset_batch_begin?: boolean;
    };
  };
};

export type SubmittedJob = {
  requestId: RequestId;
  jobId: JobId;
  runId: RunId;
};

export type AcceptedJob = {
  jobId: string;
  backend?: ActualBackend;
  device?: string;
  workerCount?: number;
  suggestedInFlight?: number;
  concurrency?: number;
};

export type OnPrepared = (job: SubmittedJob) => void;

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
  worker_count?: number;
  suggested_in_flight?: number;
  concurrency?: number;
  backend?: string;
  device?: string;
  from?: string;
  to?: string;
  reason?: string;
  frame_seq?: number;
  coverage?: {
    selection?: string;
    eligible_frames?: number;
    selected_frames?: number;
    processed_frames?: number;
    failed_frames?: number;
  };
  results?: Array<{
    seq?: number;
    error?: number | null;
    frame_index?: number;
    pts?: number | null;
    timestamp_seconds?: number | null;
  }>;
};

function wireCoverage(value: WireEvent["coverage"]): VerifyCoverage | undefined {
  if (!value) return undefined;
  if (
    value.selection !== "all" &&
    value.selection !== "decoded_i_picture" &&
    value.selection !== "every_n"
  ) {
    return undefined;
  }
  const counts = [
    value.eligible_frames,
    value.selected_frames,
    value.processed_frames,
    value.failed_frames,
  ];
  if (!counts.every((count) => Number.isSafeInteger(count) && (count as number) >= 0)) {
    return undefined;
  }
  return {
    selection: value.selection,
    eligibleFrames: value.eligible_frames as number,
    selectedFrames: value.selected_frames as number,
    processedFrames: value.processed_frames as number,
    failedFrames: value.failed_frames as number,
  };
}

type TrackedJob = {
  requestId: RequestId;
  jobId: JobId;
  runId: RunId;
  engineJobId: string | null;
  workerCount?: number;
  suggestedInFlight?: number;
  concurrency?: number;
  acceptedBackend?: ActualBackend;
  acceptedDevice?: string;
  preparedAtMs: number;
  frontendQueueMs?: number;
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
  async submitHeight(params: HeightJobParams, onPrepared?: OnPrepared): Promise<SubmittedJob> {
    return this.submitAnalyze("height", {
      kernel: params.kernel,
      candidates: params.candidates,
      frameAsset: params.frameAsset,
      axisMode: params.axisMode,
      metric: params.metric,
      backend: params.backend ?? "cpu",
      workerCount: params.workerCount,
      profileId: params.profileId ?? "muf-d278cd3",
      endpointRule: params.endpointRule ?? "inclusive",
      baseHeight: params.baseHeight ?? null,
      baseWidth: params.baseWidth ?? null,
      grid: params.grid,
      geometry: params.geometry ?? null,
    }, onPrepared);
  }

  /** Submit one kernel-mode analyze job (protocol v1.1): fixed geometry, ordered kernels. */
  async submitKernel(params: KernelJobParams, onPrepared?: OnPrepared): Promise<SubmittedJob> {
    return this.submitAnalyze("kernel", {
      kernels: params.kernels,
      candidates: [params.candidate],
      frameAsset: params.frameAsset,
      axisMode: params.axisMode,
      metric: params.metric,
      backend: params.backend ?? "cpu",
      workerCount: params.workerCount,
      profileId: params.profileId ?? "muf-d278cd3",
      endpointRule: params.endpointRule ?? "inclusive",
      baseHeight: params.baseHeight ?? null,
      baseWidth: params.baseWidth ?? null,
      geometry: params.geometry ?? null,
    }, onPrepared);
  }

  private async submitAnalyze(
    mode: "height" | "kernel",
    body: Record<string, unknown>,
    onPrepared?: OnPrepared,
  ): Promise<SubmittedJob> {
    this.sequence += 1;
    const submitted: SubmittedJob = {
      requestId: `gui-req-${this.sequence}`,
      jobId: `gui-job-${this.sequence}`,
      runId: `gui-run-${this.sequence}`,
    };
    const tracked: TrackedJob = {
      ...submitted,
      engineJobId: null,
      preparedAtMs: Date.now(),
      finished: false,
      cancelRequested: false,
    };
    this.jobsByRequest.set(submitted.requestId, tracked);
    try {
      onPrepared?.(submitted);
      await invoke("engine_worker_analyze", {
        request: {
          requestId: submitted.requestId,
          mode,
          ...body,
        },
      });
    } catch (error) {
      this.emitSubmissionFailure(tracked, error);
      this.cleanupTracked(tracked);
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

  /** Engine-owned media decode path. */
  async verifyMediaBegin(
    params: VerifyMediaJobParams,
    onPrepared?: OnPrepared,
  ): Promise<SubmittedJob> {
    this.sequence += 1;
    const submitted: SubmittedJob = {
      requestId: `gui-req-${this.sequence}`,
      jobId: `gui-job-${this.sequence}`,
      runId: `gui-run-${this.sequence}`,
    };
    const tracked: TrackedJob = {
      ...submitted,
      engineJobId: null,
      preparedAtMs: Date.now(),
      finished: false,
      cancelRequested: false,
    };
    this.jobsByRequest.set(submitted.requestId, tracked);
    try {
      onPrepared?.(submitted);
      await invoke("engine_worker_verify_media_begin", {
        request: {
          requestId: submitted.requestId,
          path: params.path,
          fingerprint: params.fingerprint ?? null,
          streamIndex: params.streamIndex,
          width: params.width,
          height: params.height,
          selection: params.selection,
          everyN: params.everyN ?? null,
          startFrame: params.startFrame ?? null,
          endFrame: params.endFrame ?? null,
          axisMode: params.axisMode,
          kernel: params.kernel,
          candidate: params.candidate,
          metric: params.metric,
          backend: params.backend ?? "auto",
          concurrency: params.concurrency,
          geometry: params.geometry ?? null,
        },
      });
    } catch (error) {
      this.emitSubmissionFailure(tracked, error);
      this.cleanupTracked(tracked);
      throw error;
    }
    return submitted;
  }

  /** Resolves with the engine-assigned job id once the job is accepted. */
  waitAccepted(requestId: RequestId): Promise<AcceptedJob> {
    const tracked = this.jobsByRequest.get(requestId);
    if (tracked?.engineJobId) {
      return Promise.resolve({
        jobId: tracked.engineJobId,
        backend: tracked.acceptedBackend,
        device: tracked.acceptedDevice,
        workerCount: tracked.workerCount,
        suggestedInFlight: tracked.suggestedInFlight,
        concurrency: tracked.concurrency,
      });
    }
    return new Promise((resolve, reject) => {
      this.acceptedWaiters.set(requestId, { resolve, reject });
    });
  }

  private acceptedWaiters = new Map<
    RequestId,
    { resolve: (accepted: AcceptedJob) => void; reject: (error: Error) => void }
  >();

  private emitSubmissionFailure(tracked: TrackedJob, error: unknown): void {
    if (tracked.finished) return;
    tracked.finished = true;
    this.emit({
      protocolVersion: 1,
      requestId: tracked.requestId,
      jobId: tracked.jobId,
      runId: tracked.runId,
      timestampMs: Date.now(),
      type: "error",
      code: "submit_failed",
      message: error instanceof Error ? error.message : String(error),
      retryable: true,
    });
  }

  private cleanupTracked(tracked: TrackedJob): void {
    this.jobsByRequest.delete(tracked.requestId);
    if (tracked.engineJobId !== null) {
      this.jobsByEngineId.delete(tracked.engineJobId);
    }
    const waiter = this.acceptedWaiters.get(tracked.requestId);
    if (waiter) {
      this.acceptedWaiters.delete(tracked.requestId);
      waiter.reject(new Error("job ended before it was accepted"));
    }
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

    // Media jobs are owned by media/service.ts and do not belong in the
    // analysis Job Tray. That service correlates these same wire events by
    // request_id and still uses the shared worker/cancel transport.
    if (wire.mode?.startsWith("media_") || wire.request_id?.startsWith("media-req-")) return;

    const tracked = this.resolveTracked(wire);
    if (wire.type === "accepted" && tracked && typeof wire.job_id === "string") {
      tracked.engineJobId = wire.job_id;
      tracked.workerCount = wire.worker_count;
      tracked.suggestedInFlight = wire.suggested_in_flight;
      tracked.concurrency = wire.concurrency;
      tracked.acceptedBackend = wireBackend(wire.backend);
      tracked.acceptedDevice = typeof wire.device === "string" ? wire.device : undefined;
      tracked.frontendQueueMs = Math.max(0, Date.now() - tracked.preparedAtMs);
      this.jobsByEngineId.set(wire.job_id, tracked);
      const waiter = this.acceptedWaiters.get(tracked.requestId);
      if (waiter) {
        this.acceptedWaiters.delete(tracked.requestId);
        waiter.resolve({
          jobId: wire.job_id,
          backend: wireBackend(wire.backend),
          device: typeof wire.device === "string" ? wire.device : undefined,
          workerCount: wire.worker_count,
          suggestedInFlight: wire.suggested_in_flight,
          concurrency: wire.concurrency,
        });
      }
      if (tracked.cancelRequested) {
        void invoke("engine_worker_cancel", { jobId: wire.job_id }).catch(() => undefined);
      }
    }
    const terminal =
      tracked && (wire.type === "result" || wire.type === "cancelled" || wire.type === "error");
    if (terminal) {
      tracked.finished = true;
      const waiter = this.acceptedWaiters.get(tracked.requestId);
      if (waiter) {
        this.acceptedWaiters.delete(tracked.requestId);
        waiter.reject(new Error(wire.message ?? `job ended with ${wire.type}`));
      }
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
        this.emit({
          ...base,
          type: "accepted",
          mode: wireMode(wire.mode),
          suggestedInFlight: wire.suggested_in_flight,
          workerCount: wire.worker_count,
          concurrency: wire.concurrency,
          backend: wireBackend(wire.backend),
          device: typeof wire.device === "string" ? wire.device : undefined,
        });
        break;
      case "progress":
        this.emit({
          ...base,
          type: "progress",
          completed: wire.completed ?? 0,
          total: wire.total ?? 0,
          detail: wire.phase ?? null,
          coverage: wireCoverage(wire.coverage),
          results: Array.isArray(wire.results)
            ? wire.results
                .filter((entry) => typeof entry?.seq === "number")
                .map((entry) => ({
                  seq: entry.seq as number,
                  error:
                    entry.error === null
                      ? null
                      : typeof entry.error === "number" && Number.isFinite(entry.error)
                        ? entry.error
                        : null,
                  frameIndex: entry.frame_index,
                  pts: entry.pts,
                  timestampSeconds: entry.timestamp_seconds,
                }))
            : undefined,
        });
        break;
      case "warning":
        this.emit({
          ...base,
          type: "warning",
          code: wire.code ?? "internal",
          message: wire.message ?? "",
          from: wire.from,
          to: wire.to,
          reason: wire.reason,
          frameSeq: wire.frame_seq,
        });
        break;
      case "result":
        this.emit({
          ...base,
          type: "result",
          mode: wireMode(wire.mode),
          coverage: wireCoverage(wire.coverage),
          payload: enrichFrontendTelemetry(
            wire.payload,
            tracked?.frontendQueueMs
              ?? (tracked ? Math.max(0, Date.now() - tracked.preparedAtMs) : undefined),
          ),
        });
        break;
      case "cancelled":
        this.emit({
          ...base,
          type: "cancelled",
          partial: wire.partial ?? false,
          coverage: wireCoverage(wire.coverage),
        });
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
    if (terminal) {
      this.cleanupTracked(tracked);
    }
  }

  private handleExit(stderrTail: string[]): void {
    // A dead worker never finishes its jobs: fail every unfinished one so the
    // Job Tray cannot show them as running forever.
    const message =
      stderrTail.length > 0
        ? stderrTail.join("\n")
        : "the engine worker exited before finishing the job";
    for (const waiter of this.acceptedWaiters.values()) {
      waiter.reject(new Error(message));
    }
    this.acceptedWaiters.clear();
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

function enrichFrontendTelemetry(payload: unknown, frontendQueueMs: number | undefined): unknown {
  if (frontendQueueMs === undefined || !payload || typeof payload !== "object") return payload;
  const record = payload as Record<string, unknown>;
  const telemetry = record.telemetry;
  if (!telemetry || typeof telemetry !== "object" || Array.isArray(telemetry)) return payload;
  return {
    ...record,
    telemetry: {
      ...(telemetry as Record<string, unknown>),
      frontend_queue_ms: frontendQueueMs,
    },
  };
}

function wireMode(mode: string | undefined): ComputationMode {
  return mode === "kernel" || mode === "verify" ? mode : "height";
}

function wireBackend(backend: string | undefined): ActualBackend | undefined {
  return backend === "cpu" || backend === "cuda" || backend === "vulkan"
    ? backend
    : undefined;
}

/** Process-wide session used by the app shell and (later) the analysis pages. */
export const engineWorker = new EngineWorkerClient();
