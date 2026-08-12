import { engineWorker, type EngineWorkerClient, type HeightJobParams } from "./workerClient";
import { materializeHeightRunGroup, type HeightRunGroupPlan } from "./runGroupPlan";
import { materializeKernelRunGroup, type KernelRunGroupPlan } from "./kernelRunGroup";
import {
  exportFrameAsset,
  exportFrameAssetBatch,
  type MediaFrameAsset,
  type MediaFrameBatchPrepareRequest,
} from "../media/service";
import { isTerminalPhase, type QueueJobInput } from "./runReducer";
import type { BackendPreference, KernelRef } from "./protocol";
import type { ProjectState, Run } from "../project/types";

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

type AssetRequest = Parameters<typeof exportFrameAsset>[0];
type AssetWork = {
  id: string;
  request: AssetRequest;
  video: boolean;
};

type BatchExporter = (
  request: MediaFrameBatchPrepareRequest,
  onAsset: Parameters<typeof exportFrameAssetBatch>[1],
) => Promise<void>;

/** Run at most two source groups together; each native producer has two
 * unacknowledged assets, so decode can overlap worker submission without an
 * unbounded frame queue. */
async function dispatchFrameAssets(input: {
  works: AssetWork[];
  enabled: boolean;
  exportAsset: typeof exportFrameAsset;
  exportBatch: BatchExporter;
  consume: (work: AssetWork, asset: MediaFrameAsset) => Promise<void>;
  fail: (work: AssetWork, error: unknown) => void;
}): Promise<void> {
  if (!input.enabled) {
    for (const work of input.works) {
      try {
        await input.consume(work, await input.exportAsset(work.request));
      } catch (error) {
        input.fail(work, error);
      }
    }
    return;
  }

  const grouped = new Map<string, AssetWork[]>();
  const singles: AssetWork[] = [];
  for (const work of input.works) {
    const request = work.request;
    if (
      !work.video || request.frameIndex == null || request.width == null ||
      request.height == null
    ) {
      singles.push(work);
      continue;
    }
    const key = JSON.stringify([
      request.path,
      request.fingerprint ?? null,
      request.streamIndex ?? 0,
      request.width,
      request.height,
    ]);
    const group = grouped.get(key) ?? [];
    group.push(work);
    grouped.set(key, group);
  }

  const units: AssetWork[][] = [...grouped.values()].flatMap((group) =>
    group.length >= 2 ? [group] : group.map((work) => [work])
  );
  units.push(...singles.map((work) => [work]));
  let nextUnit = 0;
  const runUnit = async () => {
    while (nextUnit < units.length) {
      const unit = units[nextUnit++];
      if (!unit) continue;
      const first = unit[0];
      if (!first) continue;
      if (unit.length === 1 || !first.video) {
        try {
          await input.consume(first, await input.exportAsset(first.request));
        } catch (error) {
          input.fail(first, error);
        }
        continue;
      }

      const byId = new Map(unit.map((work) => [work.id, work]));
      const delivered = new Set<string>();
      try {
        await input.exportBatch({
          path: first.request.path,
          fingerprint: first.request.fingerprint ?? null,
          streamIndex: first.request.streamIndex ?? 0,
          width: first.request.width as number,
          height: first.request.height as number,
          frames: unit.map((work) => ({
            itemId: work.id,
            frameIndex: work.request.frameIndex as number,
          })),
        }, async (asset) => {
          const work = byId.get(asset.itemId);
          if (!work) throw new Error(`frame_asset_error: unknown batch item ${asset.itemId}`);
          delivered.add(work.id);
          try {
            await input.consume(work, asset);
          } catch (error) {
            input.fail(work, error);
          }
        });
      } catch (error) {
        for (const work of unit) {
          if (!delivered.has(work.id)) input.fail(work, error);
        }
      }
    }
  };
  await Promise.all([runUnit(), runUnit()]);
}

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

/** Analyze accepts CPU, CUDA, Vulkan, and auto; Metal degrades to auto. */
export function backendForWire(
  preference: BackendPreference,
): "cpu" | "cuda" | "vulkan" | "auto" {
  return preference === "cpu" || preference === "cuda" || preference === "vulkan"
    ? preference
    : "auto";
}

export async function startHeightRunGroup(input: {
  plan: HeightRunGroupPlan;
  state: ProjectState;
  onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
  bridge: ExecutionBridge;
  worker?: Pick<EngineWorkerClient, "submitHeight">;
  exportAsset?: typeof exportFrameAsset;
  exportBatch?: BatchExporter;
  mediaFrameBatch?: boolean;
  nowMs?: () => number;
}): Promise<StartRunGroupResult> {
  const worker = input.worker ?? engineWorker;
  const exportAsset = input.exportAsset ?? exportFrameAsset;
  const exportBatch = input.exportBatch ?? exportFrameAssetBatch;
  const nowMs = input.nowMs ?? (() => Date.now());

  const { runGroup, runs } = materializeHeightRunGroup({ plan: input.plan });
  input.onProjectChange((current) => ({
    ...current,
    runGroupsById: { ...current.runGroupsById, [runGroup.id]: runGroup },
    runsById: {
      ...current.runsById,
      ...Object.fromEntries(runs.map((run) => [run.id, run])),
    },
  }));

  let submitted = 0;
  let failed = 0;
  const works: AssetWork[] = input.plan.members.map((member, index) => {
    const source = input.state.sourcesById[member.sourceId];
    return {
      id: String(index),
      video: source?.kind === "video",
      request: {
        path: member.sourcePath,
        fingerprint: member.sourceFingerprint ?? null,
        streamIndex: member.streamIndex ?? null,
        frameIndex: member.frameIndex ?? null,
        width: source?.width ?? null,
        height: source?.height ?? null,
      },
    };
  });
  const fail = (work: AssetWork, error: unknown) => {
    failed += 1;
    const message = error instanceof Error ? error.message : String(error);
    const runId = runs[Number(work.id)]?.id;
    if (!runId) return;
    input.onProjectChange((current) => {
      const run = current.runsById[runId];
      if (!run || run.status !== "queued") return current;
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
  };
  await dispatchFrameAssets({
    works,
    enabled: input.mediaFrameBatch === true,
    exportAsset,
    exportBatch,
    consume: async (work, asset) => {
      const index = Number(work.id);
      const member = input.plan.members[index];
      const projectRun = runs[index];
      if (!member || !projectRun) return;
      let prepared = false;
      try {
      await worker.submitHeight({
        frameAsset: {
          path: asset.path,
          format: "f32le",
          width: asset.width,
          height: asset.height,
        },
        axisMode: member.axisMode,
        kernel: kernelParamsForWire(member.kernel),
        candidates: member.heightGrid.candidates,
        profileId: member.profileId,
        endpointRule: member.heightGrid.endpointRule,
        baseHeight: member.request.baseHeight ?? null,
        baseWidth: member.request.baseWidth ?? null,
        grid: {
          start: member.heightGrid.start,
          stop: member.heightGrid.stop,
          step: member.heightGrid.step,
        },
        metric: {
          cropLeft: member.metric.cropLeft,
          cropRight: member.metric.cropRight,
          cropTop: member.metric.cropTop,
          cropBottom: member.metric.cropBottom,
          threshold: member.metric.pixelExclusionThreshold,
          pNorm: member.metric.pNorm,
        },
        backend: backendForWire(member.backendPreference),
      }, (job) => {
        prepared = true;
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
      });
      submitted += 1;
      } catch (error) {
        if (prepared) {
          // The client emitted a synthetic terminal event with these ids.
          failed += 1;
          return;
        }
        throw error;
      }
    },
    fail,
  });

  if (submitted === 0) {
    return { ok: false, reason: failed > 0 ? "all_members_failed" : "no_members" };
  }
  return { ok: true, runGroupId: runGroup.id, submitted, failed };
}

/**
 * Kernel RunGroup execution (GUI-4): one member per Sample, each holding one
 * fixed geometry and the full ordered kernel candidate list. The wire
 * candidate is the member geometry's active/base height. The same profile and
 * optional parity canvas dimensions are sent so the worker reconstructs the
 * resolved fractional geometry for every kernel.
 */
export async function startKernelRunGroup(input: {
  plan: KernelRunGroupPlan;
  state: ProjectState;
  onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
  bridge: ExecutionBridge;
  worker?: Pick<EngineWorkerClient, "submitKernel">;
  exportAsset?: typeof exportFrameAsset;
  exportBatch?: BatchExporter;
  mediaFrameBatch?: boolean;
  nowMs?: () => number;
}): Promise<StartRunGroupResult> {
  const worker = input.worker ?? engineWorker;
  const exportAsset = input.exportAsset ?? exportFrameAsset;
  const exportBatch = input.exportBatch ?? exportFrameAssetBatch;
  const nowMs = input.nowMs ?? (() => Date.now());

  const { runGroup, runs } = materializeKernelRunGroup({ plan: input.plan });
  input.onProjectChange((current) => ({
    ...current,
    runGroupsById: { ...current.runGroupsById, [runGroup.id]: runGroup },
    runsById: {
      ...current.runsById,
      ...Object.fromEntries(runs.map((run) => [run.id, run])),
    },
  }));

  let submitted = 0;
  let failed = 0;
  const works: AssetWork[] = input.plan.members.map((member, index) => {
    const source = input.state.sourcesById[member.sourceId];
    return {
      id: String(index),
      video: source?.kind === "video",
      request: {
        path: member.sourcePath,
        fingerprint: member.request.sourceFingerprint ?? null,
        streamIndex: member.request.streamIndex ?? null,
        frameIndex: member.request.frameIndex ?? null,
        width: source?.width ?? null,
        height: source?.height ?? null,
      },
    };
  });
  const fail = (work: AssetWork, error: unknown) => {
    failed += 1;
    const message = error instanceof Error ? error.message : String(error);
    const runId = runs[Number(work.id)]?.id;
    if (!runId) return;
    input.onProjectChange((current) => {
      const run = current.runsById[runId];
      if (!run || run.status !== "queued") return current;
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
  };
  await dispatchFrameAssets({
    works,
    enabled: input.mediaFrameBatch === true,
    exportAsset,
    exportBatch,
    consume: async (work, asset) => {
      const index = Number(work.id);
      const member = input.plan.members[index];
      const projectRun = runs[index];
      if (!member || !projectRun) return;
      const source = input.state.sourcesById[member.sourceId];
      if (!source?.width || !source.height) throw new Error("source_dims_missing");
      let prepared = false;
      try {
      await worker.submitKernel({
        frameAsset: {
          path: asset.path,
          format: "f32le",
          width: asset.width,
          height: asset.height,
        },
        axisMode: member.request.axisMode,
        candidate: String(member.geometry.baseHeight ?? member.geometry.srcHeight),
        profileId: member.request.profileId,
        endpointRule: "inclusive",
        baseHeight: member.request.geometry.baseHeight == null
          ? null
          : String(member.request.geometry.baseHeight),
        baseWidth: member.request.geometry.baseWidth == null
          ? null
          : String(member.request.geometry.baseWidth),
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
      }, (job) => {
        prepared = true;
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
      });
      submitted += 1;
      } catch (error) {
        if (prepared) {
          failed += 1;
          return;
        }
        throw error;
      }
    },
    fail,
  });

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
  if (isTerminalPhase(run.status)) {
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
