import type {
  ComputationMode,
  ActualBackend,
  JobId,
  RequestId,
  RunId,
  WorkerEvent,
} from "./protocol";

export type JobPhase =
  | "queued"
  | "running"
  | "completed"
  | "failed"
  | "cancelled"
  | "partial";

export type JobRecord = {
  id: JobId;
  requestId: RequestId;
  runId: RunId;
  runGroupId?: string | null;
  /** Persistent Project Run id this job reports into, when bound. */
  projectRunId?: string | null;
  mode: ComputationMode;
  phase: JobPhase;
  label: string;
  completed: number;
  total: number;
  warnings: Array<{ code: string; message: string }>;
  errorCode?: string | null;
  errorMessage?: string | null;
  /** True when a cancel was requested but the worker has not confirmed. */
  cancelRequested: boolean;
  /** Worker-confirmed backend; absent until accepted or with older workers. */
  backend?: ActualBackend;
  device?: string;
  /** First running timestamp; throughput stats anchor. */
  startedAtMs?: number | null;
  /** Latest engine progress phase. Plan work never contributes to throughput. */
  progressPhase?: string | null;
  /** Start of the candidates/verify phase, independent of queue and plan time. */
  rateStartedAtMs?: number | null;
  /** Measured work-phase wall time; terminal telemetry replaces event timing. */
  rateElapsedMs?: number | null;
  /** Units covered by rateElapsedMs; may differ from total when verification has failures. */
  rateCompleted?: number;
  lastProgressAtMs?: number;
  lastProgressCompleted?: number;
  /** EMA-smoothed units/sec between recent progress events. */
  fpsCurrent?: number | null;
  /** completed / elapsed since start. */
  fpsAvg?: number | null;
  updatedAtMs: number;
};

export type RunExecution = {
  id: RunId;
  requestId: RequestId;
  mode: ComputationMode;
  status: JobPhase;
  /** Immutable input snapshot id or inline marker; never mutated after create. */
  inputSnapshotKey: string;
  completed: number;
  total: number;
  /** Engine result payload when present; never fabricated by the UI. */
  result: unknown | null;
  errorCode?: string | null;
  errorMessage?: string | null;
  updatedAtMs: number;
};

export type ExecutionState = {
  jobsById: Record<string, JobRecord>;
  runsById: Record<string, RunExecution>;
  /** requestId -> jobId for cancel routing */
  requestIndex: Record<string, string>;
};

export function emptyExecutionState(): ExecutionState {
  return {
    jobsById: {},
    runsById: {},
    requestIndex: {},
  };
}

export type QueueJobInput = {
  jobId: JobId;
  requestId: RequestId;
  runId: RunId;
  runGroupId?: string | null;
  projectRunId?: string | null;
  mode: ComputationMode;
  label: string;
  total: number;
  inputSnapshotKey: string;
  nowMs: number;
};

export function queueJob(state: ExecutionState, input: QueueJobInput): ExecutionState {
  const job: JobRecord = {
    id: input.jobId,
    requestId: input.requestId,
    runId: input.runId,
    runGroupId: input.runGroupId ?? null,
    projectRunId: input.projectRunId ?? null,
    mode: input.mode,
    phase: "queued",
    label: input.label,
    completed: 0,
    total: Math.max(0, input.total),
    warnings: [],
    cancelRequested: false,
    updatedAtMs: input.nowMs,
  };
  const run: RunExecution = {
    id: input.runId,
    requestId: input.requestId,
    mode: input.mode,
    status: "queued",
    inputSnapshotKey: input.inputSnapshotKey,
    completed: 0,
    total: Math.max(0, input.total),
    result: null,
    updatedAtMs: input.nowMs,
  };
  return {
    jobsById: { ...state.jobsById, [job.id]: job },
    runsById: { ...state.runsById, [run.id]: run },
    requestIndex: { ...state.requestIndex, [input.requestId]: job.id },
  };
}

export function requestCancel(
  state: ExecutionState,
  input: { requestId?: RequestId | null; jobId?: JobId | null; nowMs: number },
): ExecutionState {
  const jobId = input.jobId ?? (input.requestId ? state.requestIndex[input.requestId] : null);
  if (!jobId) return state;
  const job = state.jobsById[jobId];
  if (!job) return state;
  if (job.phase === "completed" || job.phase === "failed" || job.phase === "cancelled") {
    return state;
  }
  return {
    ...state,
    jobsById: {
      ...state.jobsById,
      [jobId]: { ...job, cancelRequested: true, updatedAtMs: input.nowMs },
    },
  };
}

function patchJobAndRun(
  state: ExecutionState,
  event: Extract<WorkerEvent, { jobId: string; runId: string }>,
  patch: {
    phase: JobPhase;
    completed?: number;
    total?: number;
    warning?: { code: string; message: string };
    errorCode?: string | null;
    errorMessage?: string | null;
    result?: unknown | null;
    startedAtMs?: number | null;
    progressPhase?: string | null;
    rateStartedAtMs?: number | null;
    rateElapsedMs?: number | null;
    rateCompleted?: number;
    lastProgressAtMs?: number;
    lastProgressCompleted?: number;
    fpsCurrent?: number | null;
    fpsAvg?: number | null;
    backend?: ActualBackend;
    device?: string;
  },
): ExecutionState {
  const job = state.jobsById[event.jobId];
  const run = state.runsById[event.runId];
  if (!job || !run) return state;

  const nextJob: JobRecord = {
    ...job,
    phase: patch.phase,
    completed: patch.completed ?? job.completed,
    total: patch.total ?? job.total,
    warnings: patch.warning ? [...job.warnings, patch.warning] : job.warnings,
    errorCode: patch.errorCode ?? job.errorCode,
    errorMessage: patch.errorMessage ?? job.errorMessage,
    startedAtMs: patch.startedAtMs ?? job.startedAtMs ?? null,
    progressPhase:
      patch.progressPhase !== undefined ? patch.progressPhase : job.progressPhase,
    rateStartedAtMs:
      patch.rateStartedAtMs !== undefined ? patch.rateStartedAtMs : job.rateStartedAtMs,
    rateElapsedMs:
      patch.rateElapsedMs !== undefined ? patch.rateElapsedMs : job.rateElapsedMs,
    rateCompleted:
      patch.rateCompleted !== undefined ? patch.rateCompleted : job.rateCompleted,
    lastProgressAtMs: patch.lastProgressAtMs ?? job.lastProgressAtMs,
    lastProgressCompleted: patch.lastProgressCompleted ?? job.lastProgressCompleted,
    fpsCurrent: patch.fpsCurrent !== undefined ? patch.fpsCurrent : job.fpsCurrent,
    fpsAvg: patch.fpsAvg !== undefined ? patch.fpsAvg : job.fpsAvg,
    backend: patch.backend ?? job.backend,
    device: patch.device ?? job.device,
    updatedAtMs: event.timestampMs,
  };
  const nextRun: RunExecution = {
    ...run,
    status: patch.phase,
    completed: patch.completed ?? run.completed,
    total: patch.total ?? run.total,
    errorCode: patch.errorCode ?? run.errorCode,
    errorMessage: patch.errorMessage ?? run.errorMessage,
    result: patch.result !== undefined ? patch.result : run.result,
    updatedAtMs: event.timestampMs,
  };

  return {
    ...state,
    jobsById: { ...state.jobsById, [nextJob.id]: nextJob },
    runsById: { ...state.runsById, [nextRun.id]: nextRun },
  };
}

function isThroughputPhase(detail: string | null | undefined): boolean {
  // Missing detail keeps compatibility with older workers and synthetic tests.
  return detail == null || detail === "candidates" || detail === "verify";
}

function terminalRate(
  payload: unknown,
  fallbackCompleted: number,
): { completed: number; elapsedMs: number; rate: number } | null {
  if (!payload || typeof payload !== "object") return null;
  const record = payload as Record<string, unknown>;
  const telemetry = record.telemetry;
  if (!telemetry || typeof telemetry !== "object") return null;
  const values = telemetry as Record<string, unknown>;

  const candidateMs = Number(values.candidate_ms ?? values.candidates_ms);
  if (Number.isFinite(candidateMs) && candidateMs > 0) {
    const candidates = Array.isArray(record.candidates)
      ? record.candidates.length
      : fallbackCompleted;
    return {
      completed: candidates,
      elapsedMs: candidateMs,
      rate: candidates * 1000 / candidateMs,
    };
  }

  const streamMs = Number(values.stream_ms);
  const frames = Number(record.frames_completed);
  if (Number.isFinite(streamMs) && streamMs > 0 && Number.isFinite(frames) && frames >= 0) {
    return {
      completed: frames,
      elapsedMs: streamMs,
      rate: frames * 1000 / streamMs,
    };
  }
  return null;
}

/**
 * Pure reducer for worker stream events.
 * Does not invent results: `result` events must carry engine payload.
 */
export function reduceWorkerEvent(state: ExecutionState, event: WorkerEvent): ExecutionState {
  if (event.type === "hello_ok") {
    return state;
  }

  switch (event.type) {
    case "accepted": {
      const job = state.jobsById[event.jobId];
      return patchJobAndRun(state, event, {
        phase: "running",
        startedAtMs: job?.startedAtMs ?? event.timestampMs,
        backend: event.backend,
        device: event.device,
      });
    }
    case "progress": {
      const job = state.jobsById[event.jobId];
      if (!job) return state;
      if (!isThroughputPhase(event.detail)) {
        return patchJobAndRun(state, event, {
          phase: "running",
          progressPhase: event.detail ?? null,
        });
      }

      const progressPhase = event.detail ?? null;
      const phaseChanged = job.progressPhase !== progressPhase;
      const rateStartedAtMs = phaseChanged
        ? Math.min(job.updatedAtMs, event.timestampMs)
        : (job.rateStartedAtMs ?? Math.min(job.updatedAtMs, event.timestampMs));
      const previousCompleted = phaseChanged ? 0 : (job.lastProgressCompleted ?? 0);
      const monotonicCompleted = Math.max(
        phaseChanged ? 0 : job.completed,
        event.completed,
      );
      let fpsCurrent = job?.fpsCurrent ?? null;
      let lastProgressAtMs = phaseChanged ? undefined : job.lastProgressAtMs;
      let lastProgressCompleted = phaseChanged ? 0 : previousCompleted;
      if (lastProgressAtMs != null) {
        const dtSeconds = (event.timestampMs - lastProgressAtMs) / 1000;
        const delta = monotonicCompleted - previousCompleted;
        if (dtSeconds > 0 && delta > 0) {
          const instant = delta / dtSeconds;
          fpsCurrent = fpsCurrent != null ? fpsCurrent * 0.5 + instant * 0.5 : instant;
          lastProgressAtMs = event.timestampMs;
          lastProgressCompleted = monotonicCompleted;
        }
      } else {
        lastProgressAtMs = event.timestampMs;
        lastProgressCompleted = monotonicCompleted;
      }
      const elapsedMs = Math.max(
        phaseChanged ? 0 : (job.rateElapsedMs ?? 0),
        event.timestampMs - rateStartedAtMs,
      );
      const elapsedSeconds = elapsedMs / 1000;
      const fpsAvg =
        elapsedSeconds > 0 && monotonicCompleted > 0
          ? monotonicCompleted / elapsedSeconds
          : (job?.fpsAvg ?? null);
      return patchJobAndRun(state, event, {
        phase: "running",
        completed: monotonicCompleted,
        total: Math.max(job.total, event.total),
        progressPhase,
        rateStartedAtMs,
        rateElapsedMs: elapsedMs,
        rateCompleted: monotonicCompleted,
        lastProgressAtMs,
        lastProgressCompleted,
        fpsCurrent,
        fpsAvg,
      });
    }
    case "warning":
      return patchJobAndRun(state, event, {
        phase: state.jobsById[event.jobId]?.phase === "queued" ? "running" : (state.jobsById[event.jobId]?.phase ?? "running"),
        warning: { code: event.code, message: event.message },
      });
    case "result": {
      const job = state.jobsById[event.jobId];
      const completed = job?.total ?? state.runsById[event.runId]?.total ?? 0;
      const exact = terminalRate(event.payload, completed);
      return patchJobAndRun(state, event, {
        phase: "completed",
        completed,
        result: event.payload,
        rateElapsedMs: exact?.elapsedMs ?? job?.rateElapsedMs ?? null,
        rateCompleted: exact?.completed ?? job?.rateCompleted ?? completed,
        fpsAvg: exact?.rate ?? job?.fpsAvg ?? null,
      });
    }
    case "cancelled":
      return patchJobAndRun(state, event, {
        phase: event.partial ? "partial" : "cancelled",
      });
    case "error":
      return patchJobAndRun(state, event, {
        phase: "failed",
        errorCode: event.code,
        errorMessage: event.message,
      });
    default:
      return state;
  }
}

export function activeJobs(state: ExecutionState): JobRecord[] {
  return Object.values(state.jobsById)
    .filter((job) => job.phase === "queued" || job.phase === "running")
    .sort((a, b) => a.updatedAtMs - b.updatedAtMs);
}

export function recentJobs(state: ExecutionState, limit = 8): JobRecord[] {
  return Object.values(state.jobsById)
    .sort((a, b) => b.updatedAtMs - a.updatedAtMs)
    .slice(0, limit);
}

/** Aggregated per-RunGroup progress: users track the command, not member tasks. */
export type RunGroupProgress = {
  /** runGroupId, or the job id itself for ungrouped jobs. */
  id: string;
  phase: JobPhase;
  completed: number;
  total: number;
  /** Summed over currently running members. */
  fpsCurrent: number | null;
  /** Total measured work units divided by total measured work-phase wall time. */
  fpsAvg: number | null;
  rateUnit: "candidates" | "frames";
  backend?: ActualBackend;
  device?: string;
  activeJobIds: string[];
  cancelRequested: boolean;
  updatedAtMs: number;
};

const GROUP_PHASE_PRIORITY: JobPhase[] = [
  "running",
  "queued",
  "failed",
  "partial",
  "cancelled",
  "completed",
];

export function runGroupProgress(state: ExecutionState, limit = 4): RunGroupProgress[] {
  const groups = new Map<string, JobRecord[]>();
  for (const job of Object.values(state.jobsById)) {
    const key = job.runGroupId ?? job.id;
    const bucket = groups.get(key) ?? [];
    bucket.push(job);
    groups.set(key, bucket);
  }
  const progress: RunGroupProgress[] = [];
  for (const [id, jobs] of groups) {
    const phases = new Set(jobs.map((job) => job.phase));
    const phase =
      GROUP_PHASE_PRIORITY.find((candidate) => phases.has(candidate)) ?? "completed";
    const updatedAtMs = Math.max(...jobs.map((job) => job.updatedAtMs));
    const completed = jobs.reduce((sum, job) => sum + job.completed, 0);
    const total = jobs.reduce((sum, job) => sum + job.total, 0);
    const runningFps = jobs
      .filter((job) => job.phase === "running" && job.fpsCurrent != null)
      .map((job) => job.fpsCurrent as number);
    const measuredJobs = jobs.filter(
      (job) =>
        job.rateElapsedMs != null && job.rateElapsedMs > 0 && (job.rateCompleted ?? 0) > 0,
    );
    const measuredCompleted = measuredJobs.reduce(
      (sum, job) => sum + (job.rateCompleted ?? 0),
      0,
    );
    const measuredMs = measuredJobs.reduce(
      (sum, job) => sum + (job.rateElapsedMs as number),
      0,
    );
    const backends = new Set(jobs.map((job) => job.backend).filter(Boolean));
    const devices = new Set(jobs.map((job) => job.device).filter(Boolean));
    progress.push({
      id,
      phase,
      completed,
      total,
      fpsCurrent: runningFps.length > 0 ? runningFps.reduce((a, b) => a + b, 0) : null,
      fpsAvg: measuredMs > 0 ? measuredCompleted * 1000 / measuredMs : null,
      rateUnit: jobs.every((job) => job.mode === "verify") ? "frames" : "candidates",
      backend: backends.size === 1 ? [...backends][0] : undefined,
      device: devices.size === 1 ? [...devices][0] : undefined,
      activeJobIds: jobs
        .filter((job) => job.phase === "queued" || job.phase === "running")
        .map((job) => job.id),
      cancelRequested: jobs.every((job) => job.cancelRequested || (job.phase !== "queued" && job.phase !== "running")),
      updatedAtMs,
    });
  }
  return progress.sort((a, b) => b.updatedAtMs - a.updatedAtMs).slice(0, limit);
}
