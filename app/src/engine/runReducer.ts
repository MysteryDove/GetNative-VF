import type {
  ComputationMode,
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

/**
 * Pure reducer for worker stream events.
 * Does not invent results: `result` events must carry engine payload.
 */
export function reduceWorkerEvent(state: ExecutionState, event: WorkerEvent): ExecutionState {
  if (event.type === "hello_ok") {
    return state;
  }

  switch (event.type) {
    case "accepted":
      return patchJobAndRun(state, event, { phase: "running" });
    case "progress":
      return patchJobAndRun(state, event, {
        phase: "running",
        completed: event.completed,
        total: event.total,
      });
    case "warning":
      return patchJobAndRun(state, event, {
        phase: state.jobsById[event.jobId]?.phase === "queued" ? "running" : (state.jobsById[event.jobId]?.phase ?? "running"),
        warning: { code: event.code, message: event.message },
      });
    case "result":
      return patchJobAndRun(state, event, {
        phase: "completed",
        completed: state.jobsById[event.jobId]?.total ?? state.runsById[event.runId]?.total ?? 0,
        result: event.payload,
      });
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
