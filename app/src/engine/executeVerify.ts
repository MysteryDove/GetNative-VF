import { listen, type UnlistenFn } from "@tauri-apps/api/event";
import { engineWorker, type EngineWorkerClient } from "./workerClient";
import { materializeVerifyRunGroup, type VerifyRunGroupPlan } from "./verifyPlan";
import {
  abortVerifyStream,
  ackVerifyStream,
  startVerifyStream,
} from "../media/service";
import { kernelParamsForWire, backendForWire, type ExecutionBridge } from "./executeRunGroup";
import type { Recipe } from "../project/types";
import type { ProjectState, Run, RunGroup } from "../project/types";
import type { WorkerEvent } from "./protocol";

/**
 * Verification RunGroup execution (GUI-5). Per member: the streaming frame
 * producer decodes the selected scope once while the engine analyzes frames
 * frame-parallel; per-frame results stream back in progress batches and are
 * acknowledged (asset deleted) as they arrive. The orchestrator owns all
 * Project writes for its Runs — verify jobs are queued without a projectRunId
 * binding so the app-level terminal bridge skips them, letting the terminal
 * write here merge the accumulated frames into the stored result exactly once.
 */

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
    "verifyBegin" | "verifyFrame" | "verifyEnd" | "waitAccepted" | "subscribe"
  >;
  streamStart: typeof startVerifyStream;
  streamAck: typeof ackVerifyStream;
  streamAbort: typeof abortVerifyStream;
  listenAsset: (
    handler: (event: VerifyAssetPayload) => void,
  ) => Promise<UnlistenFn>;
  listenDone: (handler: (event: VerifyDonePayload) => void) => Promise<UnlistenFn>;
  listenError: (handler: (event: VerifyErrorPayload) => void) => Promise<UnlistenFn>;
  sleep: (ms: number) => Promise<void>;
  nowMs: () => number;
};

export type VerifyAssetPayload = {
  ticket: string;
  seq: number;
  frameIndex: number;
  pts?: number | null;
  timestampSeconds?: number | null;
  path: string;
  width: number;
  height: number;
};
export type VerifyDonePayload = { ticket: string; produced: number };
export type VerifyErrorPayload = { ticket: string; message: string };

function defaultDeps(): VerifyOrchestratorDeps {
  return {
    worker: engineWorker,
    streamStart: startVerifyStream,
    streamAck: ackVerifyStream,
    streamAbort: abortVerifyStream,
    listenAsset: (handler) =>
      listen<VerifyAssetPayload>("media-verify-asset", (event) => handler(event.payload)),
    listenDone: (handler) =>
      listen<VerifyDonePayload>("media-verify-stream-done", (event) => handler(event.payload)),
    listenError: (handler) =>
      listen<VerifyErrorPayload>("media-verify-stream-error", (event) => handler(event.payload)),
    sleep: (ms) => new Promise((resolve) => setTimeout(resolve, ms)),
    nowMs: () => Date.now(),
  };
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
  /** Live per-frame batches for the review surface (pre-terminal). */
  onFrames?: (projectRunId: string, entries: VerifyFrameEntry[]) => void;
  deps?: Partial<VerifyOrchestratorDeps>;
}): Promise<StartVerifyResult> {
  const deps: VerifyOrchestratorDeps = { ...defaultDeps(), ...input.deps };
  const { recipe } = input;
  if (!recipe.geometry || !recipe.kernel || !recipe.metric) {
    return { ok: false, reason: "recipe_incomplete" };
  }

  const { runGroup, runs } = materializeVerifyRunGroup({ plan: input.plan });
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
      await runVerifyMember(member, projectRun.id, recipe, runGroup.id, input, deps);
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

async function runVerifyMember(
  member: Member,
  projectRunId: string,
  recipe: Recipe,
  runGroupId: string,
  input: {
    state: ProjectState;
    onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
    bridge: ExecutionBridge;
    onFrames?: (projectRunId: string, entries: VerifyFrameEntry[]) => void;
  },
  deps: VerifyOrchestratorDeps,
): Promise<void> {
  const source = input.state.sourcesById[member.sourceId];
  if (!source?.width || !source.height) throw new Error("verify_source_dims_missing");
  const scope = member.scanScope;

  // Producer first: it reports the exact selected-frame total, which becomes
  // expected_frames. The producer pauses on the in-flight bound until the
  // engine consumes frames, so disk usage stays bounded.
  const stream = await deps.streamStart({
    path: member.request.sourcePath,
    fingerprint: member.request.sourceFingerprint ?? null,
    streamIndex: scope.streamIndex,
    selection: scope.selection,
    everyN: scope.everyN ?? null,
    startFrame: scope.startFrame ?? null,
    endFrame: scope.endFrame ?? null,
    width: source.width,
    height: source.height,
  });
  const ticket = stream.ticket;

  const frames: VerifyFrameEntry[] = [];
  const seqToFrame = new Map<number, number>();
  const seqMeta = new Map<number, { pts: number | null; timestampSeconds: number | null }>();
  let engineJobId: string | null = null;
  let sent = 0;
  let doneProduced = -1;
  let streamError: string | null = null;

  let resolveTerminal!: (event: TerminalEvent) => void;
  const terminal = new Promise<TerminalEvent>((resolve) => {
    resolveTerminal = resolve;
  });
  let myRunId: string | null = null;

  const unlistenWorker = deps.worker.subscribe((event: WorkerEvent) => {
    if (!myRunId || !("runId" in event) || event.runId !== myRunId) return;
    if (event.type === "progress" && event.results?.length) {
      const batch: VerifyFrameEntry[] = [];
      for (const entry of event.results) {
        const frameIndex = seqToFrame.get(entry.seq) ?? entry.seq;
        const meta = seqMeta.get(entry.seq);
        const record = {
          seq: entry.seq,
          frameIndex,
          pts: meta?.pts ?? null,
          timestampSeconds: meta?.timestampSeconds ?? null,
          error: entry.error,
        };
        frames.push(record);
        batch.push(record);
        void deps.streamAck(ticket, entry.seq).catch(() => undefined);
      }
      input.onFrames?.(projectRunId, batch);
      return;
    }
    if (event.type === "result" || event.type === "cancelled" || event.type === "error") {
      resolveTerminal(event);
    }
  });

  const unlistenAsset = await deps.listenAsset((asset) => {
    if (asset.ticket !== ticket || !engineJobId) return;
    seqToFrame.set(asset.seq, asset.frameIndex);
    seqMeta.set(asset.seq, { pts: asset.pts ?? null, timestampSeconds: asset.timestampSeconds ?? null });
    sent += 1;
    const jobId = engineJobId;
    deps.worker
      .verifyFrame({
        jobId,
        seq: asset.seq,
        frameAsset: {
          path: asset.path,
          format: "f32le",
          width: asset.width,
          height: asset.height,
        },
      })
      .catch(() => undefined);
  });
  const unlistenDone = await deps.listenDone((done) => {
    if (done.ticket === ticket) doneProduced = done.produced;
  });
  const unlistenError = await deps.listenError((failure) => {
    if (failure.ticket === ticket) streamError = failure.message;
  });

  try {
    const submitted = await deps.worker.verifyBegin({
      width: stream.width,
      height: stream.height,
      axisMode: "h_only",
      kernel: kernelParamsForWire(recipe.kernel!),
      candidate: String(recipe.geometry!.canvasHeight),
      metric: {
        cropLeft: recipe.metric!.cropLeft,
        cropRight: recipe.metric!.cropRight,
        cropTop: recipe.metric!.cropTop,
        cropBottom: recipe.metric!.cropBottom,
        threshold: recipe.metric!.pixelExclusionThreshold,
        pNorm: recipe.metric!.pNorm,
      },
      // Verify is CPU-only in protocol v1.1; cuda degrades to the default.
      backend: backendForWire(member.request.backendPreference) === "cuda" ? "auto" : "cpu",
      expectedFrames: stream.total,
    });
    myRunId = submitted.runId;

    input.bridge.queue({
      jobId: submitted.jobId,
      requestId: submitted.requestId,
      runId: submitted.runId,
      runGroupId,
      projectRunId: null,
      mode: "verify",
      label: member.sourceLabel,
      total: Number(stream.total),
      inputSnapshotKey: member.planKey,
      nowMs: deps.nowMs(),
    });

    engineJobId = await deps.worker.waitAccepted(submitted.requestId);

    // Drive the stream to completion, then close honestly with the number of
    // frames actually sent. A terminal event (error/cancel) wins the race.
    const drive = (async () => {
      while (doneProduced < 0 && streamError === null) {
        await deps.sleep(50);
      }
      if (streamError !== null) throw new Error(streamError);
      await deps.worker.verifyEnd({ jobId: engineJobId as string, total: sent });
    })();
    const outcome = await Promise.race([drive.then(() => null), terminal]);
    if (outcome === null) {
      // Stream closed; wait for the engine's terminal event.
      await terminal;
    } else {
      // Terminal arrived mid-stream: stop the producer and skip verify_end.
      await deps.streamAbort(ticket).catch(() => undefined);
    }
    const event = await terminal;

    const nowIso = new Date(deps.nowMs()).toISOString();
    const sorted = [...frames].sort((a, b) => a.seq - b.seq);
    input.onProjectChange((current) => {
      const run = current.runsById[projectRunId];
      if (!run) return current;
      if (run.status === "completed" || run.status === "failed" || run.status === "cancelled" || run.status === "partial") {
        return current;
      }
      const next: Run = { ...run, updatedAt: nowIso, completed: sorted.length };
      if (event.type === "result") {
        next.status = "completed";
        next.completed = run.total > 0 ? run.total : sorted.length;
        const payload = (event.payload ?? {}) as Record<string, unknown>;
        next.result = { ...payload, frames: sorted };
      } else if (event.type === "cancelled") {
        next.status = event.partial ? "partial" : "cancelled";
        if (sorted.length) {
          next.result = { mode: "verify", frames: sorted };
        }
      } else {
        next.status = "failed";
        next.errorCode = event.code;
        next.errorMessage = event.message;
      }
      return {
        ...current,
        runsById: { ...current.runsById, [projectRunId]: next },
      };
    });
  } finally {
    unlistenWorker();
    unlistenAsset();
    unlistenDone();
    unlistenError();
    void deps.streamAbort(ticket).catch(() => undefined);
  }
}
