import { engineWorker, type EngineWorkerClient, type HeightJobParams } from "./workerClient";
import { materializeHeightRunGroup, type HeightRunGroupPlan } from "./runGroupPlan";
import { materializeKernelRunGroup, type KernelRunGroupPlan } from "./kernelRunGroup";
import { exportFrameAsset } from "../media/service";
import type { QueueJobInput } from "./runReducer";
import type { BackendPreference, KernelRef } from "./protocol";
import type { ProjectState, Run, RunGroup } from "../project/types";

/**
 * RunGroup execution orchestration (GUI-3): materialize the plan into
 * immutable Project records, export one frame asset per member, submit each
 * member to the resident worker, and bind live job state to the persistent
 * Run records. Engine results land on Runs only via worker terminal events
 * (App-level bridge) — never from here.
 */

export type ExecutionBridge = {
  queue(input: QueueJobInput): void;
  cancel(jobId: string): void;
};

export type StartRunGroupResult =
  | { ok: true; runGroupId: string; submitted: number; failed: number }
  | { ok: false; reason: string };

/** Map a semantic KernelRef onto the wire kernel shape (numbers, relevant params only). */
export function kernelParamsForWire(kernel: KernelRef): HeightJobParams["kernel"] {
  const wire: { id: string; b?: number; c?: number; taps?: number } = { id: kernel.id };
  if (kernel.id === "bicubic") {
    const b = Number(kernel.parameters.b);
    const c = Number(kernel.parameters.c);
    if (Number.isFinite(b)) wire.b = b;
    if (Number.isFinite(c)) wire.c = c;
  } else if (kernel.id === "lanczos") {
    const taps = Number(kernel.parameters.taps);
    if (Number.isFinite(taps)) wire.taps = taps;
  }
  return wire;
}

/** The worker accepts cpu/cuda/auto; a Metal preference degrades to auto explicitly. */
export function backendForWire(preference: BackendPreference): "cpu" | "cuda" | "auto" {
  return preference === "cpu" || preference === "cuda" ? preference : "auto";
}

export async function startHeightRunGroup(input: {
  plan: HeightRunGroupPlan;
  state: ProjectState;
  onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
  bridge: ExecutionBridge;
  worker?: Pick<EngineWorkerClient, "submitHeight">;
  exportAsset?: typeof exportFrameAsset;
  nowMs?: () => number;
}): Promise<StartRunGroupResult> {
  const worker = input.worker ?? engineWorker;
  const exportAsset = input.exportAsset ?? exportFrameAsset;
  const nowMs = input.nowMs ?? (() => Date.now());

  const { runGroup, runs } = materializeHeightRunGroup({ plan: input.plan });
  input.onProjectChange((current) => ({
    ...current,
    runGroupsById: { ...current.runGroupsById, [runGroup.id]: runGroup as RunGroup },
    runsById: {
      ...current.runsById,
      ...Object.fromEntries(runs.map((run) => [run.id, run as Run])),
    },
  }));

  let submitted = 0;
  let failed = 0;
  for (const [index, member] of input.plan.members.entries()) {
    const projectRun = runs[index];
    if (!projectRun) continue;
    const source = input.state.sourcesById[member.sourceId];
    try {
      const asset = await exportAsset({
        path: member.sourcePath,
        fingerprint: member.sourceFingerprint ?? null,
        streamIndex: member.streamIndex ?? null,
        frameIndex: member.frameIndex ?? null,
        width: source?.width ?? null,
        height: source?.height ?? null,
      });
      const job = await worker.submitHeight({
        frameAsset: {
          path: asset.path,
          format: "f32le",
          width: asset.width,
          height: asset.height,
        },
        axisMode: member.axisMode,
        kernel: kernelParamsForWire(member.kernel),
        candidates: member.heightGrid.candidates,
        metric: {
          cropLeft: member.metric.cropLeft,
          cropRight: member.metric.cropRight,
          cropTop: member.metric.cropTop,
          cropBottom: member.metric.cropBottom,
          threshold: member.metric.pixelExclusionThreshold,
          pNorm: member.metric.pNorm,
        },
        backend: backendForWire(member.backendPreference),
      });
      input.bridge.queue({
        jobId: job.jobId,
        requestId: job.requestId,
        runId: job.runId,
        runGroupId: runGroup.id,
        projectRunId: projectRun.id,
        mode: "height",
        label: `${member.sampleLabel} · ${member.kernel.id}`,
        total: member.heightGrid.candidates.length,
        inputSnapshotKey: member.planKey,
        nowMs: nowMs(),
      });
      submitted += 1;
    } catch (error) {
      // Member independence: a failed frame asset or submission marks that
      // member's Run failed; the rest of the group still runs.
      failed += 1;
      const message = error instanceof Error ? error.message : String(error);
      const runId = projectRun.id;
      input.onProjectChange((current) => {
        const run = current.runsById[runId];
        if (!run) return current;
        return {
          ...current,
          runsById: {
            ...current.runsById,
            [runId]: {
              ...run,
              status: "failed",
              errorCode: message.includes("frame_asset") ? "frame_asset_error" : "submit_failed",
              errorMessage: message,
              updatedAt: new Date(nowMs()).toISOString(),
            },
          },
        };
      });
    }
  }

  if (submitted === 0) {
    return { ok: false, reason: failed > 0 ? "all_members_failed" : "no_members" };
  }
  return { ok: true, runGroupId: runGroup.id, submitted, failed };
}

/**
 * Kernel RunGroup execution (GUI-4): one member per Sample, each holding one
 * fixed geometry and the full ordered kernel candidate list. The wire
 * candidate is the member geometry's canvas height (integer canvas semantics
 * match the engine's floor rule exactly); fractional-active kernel scans wait
 * for the engine's shift support, like fractional height scans.
 */
export async function startKernelRunGroup(input: {
  plan: KernelRunGroupPlan;
  state: ProjectState;
  onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
  bridge: ExecutionBridge;
  worker?: Pick<EngineWorkerClient, "submitKernel">;
  exportAsset?: typeof exportFrameAsset;
  nowMs?: () => number;
}): Promise<StartRunGroupResult> {
  const worker = input.worker ?? engineWorker;
  const exportAsset = input.exportAsset ?? exportFrameAsset;
  const nowMs = input.nowMs ?? (() => Date.now());

  const { runGroup, runs } = materializeKernelRunGroup({ plan: input.plan });
  input.onProjectChange((current) => ({
    ...current,
    runGroupsById: { ...current.runGroupsById, [runGroup.id]: runGroup as RunGroup },
    runsById: {
      ...current.runsById,
      ...Object.fromEntries(runs.map((run) => [run.id, run as Run])),
    },
  }));

  let submitted = 0;
  let failed = 0;
  for (const [index, member] of input.plan.members.entries()) {
    const projectRun = runs[index];
    if (!projectRun) continue;
    try {
      const asset = await exportAsset({
        path: member.sourcePath,
        fingerprint: member.request.sourceFingerprint ?? null,
        streamIndex: member.request.streamIndex ?? null,
        frameIndex: member.request.frameIndex ?? null,
        width: member.geometry.activeWidth,
        height: member.geometry.activeHeight,
      });
      const job = await worker.submitKernel({
        frameAsset: {
          path: asset.path,
          format: "f32le",
          width: asset.width,
          height: asset.height,
        },
        axisMode: "h_only",
        candidate: String(member.geometry.canvasHeight),
        kernels: member.kernels.map((kernel) => kernelParamsForWire(kernel)),
        metric: {
          cropLeft: member.request.metric.cropLeft,
          cropRight: member.request.metric.cropRight,
          cropTop: member.request.metric.cropTop,
          cropBottom: member.request.metric.cropBottom,
          threshold: member.request.metric.pixelExclusionThreshold,
          pNorm: member.request.metric.pNorm,
        },
        backend: backendForWire(member.request.backendPreference),
      });
      input.bridge.queue({
        jobId: job.jobId,
        requestId: job.requestId,
        runId: job.runId,
        runGroupId: runGroup.id,
        projectRunId: projectRun.id,
        mode: "kernel",
        label: `${member.sampleLabel} · ${member.kernels.length} kernels`,
        total: member.kernels.length,
        inputSnapshotKey: member.planKey,
        nowMs: nowMs(),
      });
      submitted += 1;
    } catch (error) {
      failed += 1;
      const message = error instanceof Error ? error.message : String(error);
      const runId = projectRun.id;
      input.onProjectChange((current) => {
        const run = current.runsById[runId];
        if (!run) return current;
        return {
          ...current,
          runsById: {
            ...current.runsById,
            [runId]: {
              ...run,
              status: "failed",
              errorCode: message.includes("frame_asset") ? "frame_asset_error" : "submit_failed",
              errorMessage: message,
              updatedAt: new Date(nowMs()).toISOString(),
            },
          },
        };
      });
    }
  }

  if (submitted === 0) {
    return { ok: false, reason: failed > 0 ? "all_members_failed" : "no_members" };
  }
  return { ok: true, runGroupId: runGroup.id, submitted, failed };
}

/**
 * Apply a worker terminal event to the persistent Project Run. Append-only:
 * a Run that already reached a terminal status is never rewritten.
 */
export function applyTerminalEventToRun(
  state: ProjectState,
  projectRunId: string,
  event: { type: "result" | "cancelled" | "error"; payload?: unknown; code?: string; message?: string; partial?: boolean },
  nowIso: string,
): ProjectState {
  const run = state.runsById[projectRunId];
  if (!run) return state;
  if (run.status === "completed" || run.status === "failed" || run.status === "cancelled" || run.status === "partial") {
    return state;
  }
  const next: Run = { ...run, updatedAt: nowIso };
  if (event.type === "result") {
    next.status = "completed";
    next.completed = run.total > 0 ? run.total : run.completed;
    next.result = event.payload ?? null;
  } else if (event.type === "cancelled") {
    next.status = event.partial ? "partial" : "cancelled";
    if (event.partial && event.payload !== undefined) {
      next.result = event.payload ?? null;
    }
  } else {
    next.status = "failed";
    next.errorCode = event.code ?? "internal";
    next.errorMessage = event.message ?? "";
  }
  return {
    ...state,
    runsById: { ...state.runsById, [projectRunId]: next },
  };
}
