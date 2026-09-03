import { engineWorker, type EngineWorkerClient } from "./workerClient";
import { materializeVerifyRunGroup, type VerifyRunGroupPlan } from "./verifyPlan";
import { kernelParamsForWire, type ExecutionBridge } from "./executeRunGroup";
import { isTerminalPhase } from "./runReducer";
import type { ProjectState, Recipe, Run } from "../project/types";
import type { VerifyCoverage, WorkerEvent } from "./protocol";
import { geometryCandidate, geometryToWire } from "./geometry";

export type VerifyFrameEntry = {
  seq: number;
  frameIndex: number;
  pts?: number | null;
  timestampSeconds?: number | null;
  error: number | null;
};

export type VerifyOrchestratorDeps = {
  worker: Pick<
    EngineWorkerClient,
    "verifyMediaBegin" | "waitAccepted" | "subscribe" | "cancel"
  >;
  nowMs: () => number;
};

function defaultDeps(): VerifyOrchestratorDeps {
  return { worker: engineWorker, nowMs: () => Date.now() };
}

export type StartVerifyResult =
  | { ok: true; runGroupId: string; submitted: number; failed: number }
  | { ok: false; reason: string };

export async function startVerifyRunGroup(input: {
  plan: VerifyRunGroupPlan;
  recipe: Recipe;
  state: ProjectState;
  onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
  bridge: ExecutionBridge;
  onFrames?: (projectRunId: string, entries: VerifyFrameEntry[]) => void;
  onTerminal?: (projectRunId: string) => void;
  deps?: Partial<VerifyOrchestratorDeps>;
}): Promise<StartVerifyResult> {
  const deps = { ...defaultDeps(), ...input.deps };
  const { recipe } = input;
  if (!recipe.geometry || !recipe.kernel || !recipe.metric) {
    return { ok: false, reason: "recipe_incomplete" };
  }

  const { runGroup, runs } = materializeVerifyRunGroup({ plan: input.plan });
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
  for (const [index, member] of input.plan.members.entries()) {
    const projectRun = runs[index];
    if (!projectRun) continue;
    try {
      const source = input.state.sourcesById[member.sourceId];
      if (!source?.width || !source.height) throw new Error("verify_source_dims_missing");
      await runEngineMediaVerifyMember(
        member, projectRun.id, recipe, runGroup.id,
        source.width, source.height, input, deps,
      );
      submitted += 1;
    } catch (error) {
      failed += 1;
      const message = error instanceof Error ? error.message : String(error);
      const nowIso = new Date(deps.nowMs()).toISOString();
      input.onProjectChange((current) => {
        const run = current.runsById[projectRun.id];
        if (!run) return current;
        return {
          ...current,
          runsById: {
            ...current.runsById,
            [projectRun.id]: {
              ...run,
              status: "failed",
              errorCode: message.includes("video_backend_unavailable")
                ? "video_backend_unavailable"
                : "verify_failed",
              errorMessage: message,
              updatedAt: nowIso,
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

type Member = VerifyRunGroupPlan["members"][number];
type TerminalEvent = Extract<WorkerEvent, { type: "result" | "cancelled" | "error" }>;

async function runEngineMediaVerifyMember(
  member: Member,
  projectRunId: string,
  recipe: Recipe,
  runGroupId: string,
  width: number,
  height: number,
  input: {
    onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
    bridge: ExecutionBridge;
    onFrames?: (projectRunId: string, entries: VerifyFrameEntry[]) => void;
    onTerminal?: (projectRunId: string) => void;
  },
  deps: VerifyOrchestratorDeps,
): Promise<void> {
  const frames: VerifyFrameEntry[] = [];
  let framesSeen = 0;
  let exactTotal = 0;
  let latestCoverage: VerifyCoverage | undefined;
  let myRunId: string | null = null;
  let resolveTerminal!: (event: TerminalEvent) => void;
  const terminal = new Promise<TerminalEvent>((resolve) => {
    resolveTerminal = resolve;
  });
  const unlisten = deps.worker.subscribe((event) => {
    if (!myRunId || !("runId" in event) || event.runId !== myRunId) return;
    if (event.type === "progress") {
      if (event.coverage) latestCoverage = event.coverage;
      if (event.total > 0 && event.total !== exactTotal) {
        exactTotal = event.total;
        input.onProjectChange((current) => {
          const run = current.runsById[projectRunId];
          if (!run || run.status !== "queued") return current;
          return {
            ...current,
            runsById: {
              ...current.runsById,
              [projectRunId]: { ...run, total: event.total },
            },
          };
        });
      }
      if (event.results?.length) {
        const batch = event.results.map((entry) => ({
          seq: entry.seq,
          frameIndex: entry.frameIndex ?? entry.seq,
          pts: entry.pts ?? null,
          timestampSeconds: entry.timestampSeconds ?? null,
          error: entry.error,
        }));
        framesSeen += batch.length;
        frames.push(...batch);
        input.onFrames?.(projectRunId, batch);
      }
      return;
    }
    if (event.type === "result" || event.type === "cancelled" || event.type === "error") {
      resolveTerminal(event);
    }
  });

  try {
    let acceptedPromise: ReturnType<EngineWorkerClient["waitAccepted"]> | null = null;
    const backend = member.request.backendPreference;
    const wireBackend = backend === "cpu" || backend === "cuda" || backend === "vulkan" || backend === "metal"
      ? backend
      : "auto";
    const submitted = await deps.worker.verifyMediaBegin({
      path: member.request.sourcePath,
      fingerprint: member.request.sourceFingerprint ?? null,
      streamIndex: member.scanScope.streamIndex,
      width,
      height,
      selection: member.scanScope.selection,
      everyN: member.scanScope.everyN ?? null,
      startFrame: member.scanScope.startFrame ?? null,
      endFrame: member.scanScope.endFrame ?? null,
      axisMode: member.request.axisMode,
      kernel: kernelParamsForWire(recipe.kernel!),
      candidate: String(geometryCandidate(member.request.geometry, member.request.axisMode)),
      geometry: geometryToWire(member.request.geometry),
      metric: {
        cropLeft: member.request.metric.cropLeft,
        cropRight: member.request.metric.cropRight,
        cropTop: member.request.metric.cropTop,
        cropBottom: member.request.metric.cropBottom,
        threshold: member.request.metric.pixelExclusionThreshold,
        pNorm: member.request.metric.pNorm,
      },
      backend: wireBackend,
      concurrency: member.request.concurrency,
    }, (job) => {
      myRunId = job.runId;
      acceptedPromise = deps.worker.waitAccepted(job.requestId);
      input.bridge.queue({
        jobId: job.jobId,
        requestId: job.requestId,
        runId: job.runId,
        runGroupId,
        projectRunId: null,
        mode: "verify",
        label: member.sourceLabel,
        total: 0,
        inputSnapshotKey: member.planKey,
        nowMs: deps.nowMs(),
      });
    });
    myRunId = submitted.runId;
    await (acceptedPromise ?? deps.worker.waitAccepted(submitted.requestId));
    const event = await terminal;
    const sorted = [...frames].sort((left, right) => left.seq - right.seq);
    const coverage = "coverage" in event ? event.coverage ?? latestCoverage : latestCoverage;
    const nowIso = new Date(deps.nowMs()).toISOString();
    input.onProjectChange((current) => {
      const run = current.runsById[projectRunId];
      if (!run || isTerminalPhase(run.status)) {
        return current;
      }
      const next: Run = {
        ...run,
        total: exactTotal || run.total,
        completed: framesSeen,
        updatedAt: nowIso,
      };
      if (event.type === "result") {
        next.status = "completed";
        next.result = {
          ...((event.payload ?? {}) as Record<string, unknown>),
          frames: sorted,
          ...(coverage ? { coverage } : {}),
        };
      } else if (event.type === "cancelled") {
        next.status = event.partial ? "partial" : "cancelled";
        if (sorted.length || coverage) {
          next.result = {
            mode: "verify",
            frames: sorted,
            ...(coverage ? { coverage } : {}),
          };
        }
      } else {
        next.status = "failed";
        next.errorCode = event.code;
        next.errorMessage = event.message;
        if (sorted.length || coverage) {
          next.result = {
            mode: "verify",
            frames: sorted,
            ...(coverage ? { coverage } : {}),
          };
        }
      }
      return {
        ...current,
        runsById: { ...current.runsById, [projectRunId]: next },
      };
    });
    input.onTerminal?.(projectRunId);
  } finally {
    unlisten();
  }
}
