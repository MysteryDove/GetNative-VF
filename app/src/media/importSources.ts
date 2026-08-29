import type { ProjectState, Sample, Source } from "../project/types";
import {
  beginMediaIndex,
  probeMedia,
  type MediaIndexResult,
  type MediaProbeResult,
  type MediaTask,
} from "./service";
import { findDuplicateSampleId } from "./frameBrowser";

/**
 * Shared media import pipeline (Media page picker/drop, Samples page drop).
 * Registers provisional probing Sources, probes each, and merges results with
 * path+fingerprint dedup. Optionally creates included Samples for still
 * images (Samples page drop flow); videos import as Sources only — frame
 * selection stays on the Media page.
 */

export type ProjectUpdater = (updater: (state: ProjectState) => ProjectState) => void;

export function fileName(path: string): string {
  return path.split(/[\\/]/).pop() || path;
}

export function sourceFromProbe(id: string, probe: MediaProbeResult, label?: string | null): Source {
  return {
    id,
    kind: probe.kind,
    path: probe.path,
    fingerprint: probe.fingerprint,
    state: probe.state,
    label: label ?? probe.file_name,
    sizeBytes: probe.size_bytes,
    width: probe.width,
    height: probe.height,
    durationSeconds: probe.duration_seconds,
    decoder: probe.decoder,
    videoStreams: probe.video_streams.map((stream) => ({
      index: stream.index,
      codecName: stream.codec_name,
      width: stream.width,
      height: stream.height,
      durationSeconds: stream.duration_seconds,
      frameCount: stream.frame_count,
      timeBaseNum: stream.time_base_num,
      timeBaseDen: stream.time_base_den,
      frameRateNum: stream.frame_rate_num,
      frameRateDen: stream.frame_rate_den,
    })),
    selectedStreamIndex: probe.selected_stream_index,
    errorCode: probe.diagnostic?.code ?? null,
    errorDetail: probe.diagnostic?.detail ?? null,
  };
}

export function mergeProbedSource(
  sourcesById: Record<string, Source>,
  provisionalId: string,
  probe: MediaProbeResult,
): Record<string, Source> {
  const duplicate = Object.values(sourcesById).find(
    (source) =>
      source.id !== provisionalId &&
      source.path === probe.path &&
      source.fingerprint === probe.fingerprint,
  );
  if (duplicate) {
    const next = { ...sourcesById };
    delete next[provisionalId];
    return next;
  }
  return {
    ...sourcesById,
    [provisionalId]: sourceFromProbe(provisionalId, probe),
  };
}

/** After merging, the surviving record may be the provisional id or a dedup target. */
export function finalSourceIdFor(
  sourcesById: Record<string, Source>,
  provisionalId: string,
  probe: MediaProbeResult,
): string | null {
  if (sourcesById[provisionalId]) return provisionalId;
  const duplicate = Object.values(sourcesById).find(
    (source) => source.path === probe.path && source.fingerprint === probe.fingerprint,
  );
  return duplicate?.id ?? null;
}

/** Create an included Sample for a ready still Source, or null when it would duplicate. */
export function stillSampleForSource(
  state: ProjectState,
  sourceId: string,
  idFactory: () => string = () => crypto.randomUUID(),
): Sample | null {
  const source = state.sourcesById[sourceId];
  if (!source || source.kind !== "still" || source.state !== "ready") return null;
  const duplicate = findDuplicateSampleId(Object.values(state.samplesById), {
    sourceId,
    kind: "still",
  });
  if (duplicate) return null;
  const order =
    Math.max(-1, ...Object.values(state.samplesById).map((sample) => sample.order)) + 1;
  return {
    id: `smp_${idFactory()}`,
    sourceId,
    sourceFingerprint: source.fingerprint ?? null,
    label: source.label ?? fileName(source.path),
    included: true,
    order,
    frameIndex: null,
    streamIndex: null,
    pts: null,
    bestEffortTimestamp: null,
    timeBaseNum: null,
    timeBaseDen: null,
    timestampSeconds: null,
    tags: [],
  };
}

export type ImportOutcome = {
  pending: number;
  sampled: number;
  videos: number;
  failed: number;
};

const activeVideoIndexes = new Map<string, Set<MediaTask<MediaIndexResult>>>();

export async function cancelSourceImport(sourceId: string): Promise<void> {
  const tasks = activeVideoIndexes.get(sourceId);
  if (!tasks) return;
  await Promise.all([...tasks].map((task) => task.cancel()));
}

export function beginSourceMediaIndex(
  sourceId: string,
  request: Parameters<typeof beginMediaIndex>[0],
  onProgress?: Parameters<typeof beginMediaIndex>[1],
  start: typeof beginMediaIndex = beginMediaIndex,
): MediaTask<MediaIndexResult> {
  const base = start(request, onProgress);
  let tracked: MediaTask<MediaIndexResult>;
  tracked = {
    ...base,
    promise: base.promise.finally(() => {
      const tasks = activeVideoIndexes.get(sourceId);
      tasks?.delete(tracked);
      if (tasks?.size === 0) {
        activeVideoIndexes.delete(sourceId);
      }
    }),
  };
  const tasks = activeVideoIndexes.get(sourceId) ?? new Set();
  tasks.add(tracked);
  activeVideoIndexes.set(sourceId, tasks);
  return tracked;
}

export function indexedVideoProbe(probe: MediaProbeResult, index: MediaIndexResult): MediaProbeResult {
  return {
    ...probe,
    kind: "video",
    state: "ready",
    fingerprint: index.fingerprint,
    size_bytes: index.size_bytes,
    width: index.width,
    height: index.height,
    duration_seconds: index.duration_seconds,
    decoder: index.decoder,
    selected_stream_index: index.stream_index,
    diagnostic: null,
    video_streams: [{
      index: index.stream_index,
      codec_name: index.codec,
      width: index.width,
      height: index.height,
      duration_seconds: index.duration_seconds,
      frame_count: index.frame_count,
      time_base_num: index.time_base_num,
      time_base_den: index.time_base_den,
      frame_rate_num: null,
      frame_rate_den: null,
    }],
  };
}

/**
 * Relink flow (Media page): after a successful re-probe, replace the Source
 * record and, for videos still probing, run the media index and swap in the
 * indexed record. `onIndexProgress` reports decoded-frame counts.
 */
export async function applyRelinkedProbe(input: {
  sourceId: string;
  probe: MediaProbeResult;
  label?: string | null;
  onProjectChange: ProjectUpdater;
  onIndexProgress?: (sourceId: string, decodedFrames: number) => void;
}): Promise<void> {
  const { sourceId, probe, label, onProjectChange } = input;
  onProjectChange((current) => ({
    ...current,
    sourcesById: {
      ...current.sourcesById,
      [sourceId]: sourceFromProbe(sourceId, probe, label),
    },
  }));
  if (probe.kind !== "video" || probe.state !== "probing") return;
  const task = beginSourceMediaIndex(
    sourceId,
    { path: probe.path, fingerprint: probe.fingerprint },
    (progress) => input.onIndexProgress?.(sourceId, progress.completed),
  );
  const indexed = indexedVideoProbe(probe, await task.promise);
  onProjectChange((current) => current.sourcesById[sourceId]
    ? {
        ...current,
        sourcesById: {
          ...current.sourcesById,
          [sourceId]: sourceFromProbe(sourceId, indexed, label),
        },
      }
    : current);
}

function mediaErrorCode(error: unknown): string {  const detail = String(error).replace(/^Error:\s*/, "");
  const match = detail.match(/^([a-z][a-z0-9_]+):/);
  const code = match?.[1] ?? "media_index_error";
  return code === "media_fingerprint_error"
    ? "media_fingerprint_mismatch"
    : code;
}

export async function importMediaPaths(input: {
  paths: string[];
  state: ProjectState;
  onProjectChange: ProjectUpdater;
  /** Create included Samples for still images (Samples page flow). */
  createSamplesForStills?: boolean;
  /** Called synchronously with the first provisional Source id (e.g. to select it). */
  onPending?: (provisionalId: string) => void;
  onBusyDelta?: (delta: number) => void;
  onIndexProgress?: (sourceId: string, decodedFrames: number) => void;
}): Promise<ImportOutcome> {
  const outcome: ImportOutcome = { pending: 0, sampled: 0, videos: 0, failed: 0 };
  const { state, onProjectChange } = input;
  if (state.project.readOnly) return outcome;
  const knownPaths = new Set(Object.values(state.sourcesById).map((source) => source.path));
  const uniquePaths = input.paths.filter((path) => {
    if (!path || knownPaths.has(path)) return false;
    knownPaths.add(path);
    return true;
  });
  if (!uniquePaths.length) return outcome;

  const records = uniquePaths.map((path) => ({
    id: `src_${crypto.randomUUID()}`,
    kind: "still" as const,
    path,
    state: "probing" as const,
    label: fileName(path),
    videoStreams: [],
  }));
  onProjectChange((current) => ({
    ...current,
    sourcesById: {
      ...current.sourcesById,
      ...Object.fromEntries(records.map((source) => [source.id, source])),
    },
  }));
  if (records[0]) input.onPending?.(records[0].id);
  outcome.pending = records.length;
  input.onBusyDelta?.(records.length);

  // Local snapshots make outcome accounting exact and keep every updater
  // idempotent (safe under StrictMode double-invocation).
  let sourcesSnapshot = state.sourcesById;
  let samplesSnapshot = state.samplesById;

  await Promise.all(
    records.map(async (record) => {
      try {
        const probe = await probeMedia(record.path);
        onProjectChange((current) => ({
          ...current,
          sourcesById: mergeProbedSource(current.sourcesById, record.id, probe),
        }));
        sourcesSnapshot = mergeProbedSource(sourcesSnapshot, record.id, probe);
        let completedProbe = probe;
        if (probe.kind === "video" && probe.state === "probing") {
          outcome.videos += 1;
          const task = beginSourceMediaIndex(
            record.id,
            { path: probe.path, fingerprint: probe.fingerprint },
            (progress) => input.onIndexProgress?.(record.id, progress.completed),
          );
          completedProbe = indexedVideoProbe(probe, await task.promise);
          onProjectChange((current) => {
            if (!current.sourcesById[record.id]) return current;
            return {
              ...current,
              sourcesById: mergeProbedSource(current.sourcesById, record.id, completedProbe),
            };
          });
          sourcesSnapshot = mergeProbedSource(sourcesSnapshot, record.id, completedProbe);
        }
        if (input.createSamplesForStills && probe.kind === "still" && probe.state === "ready") {
          const finalId = finalSourceIdFor(sourcesSnapshot, record.id, completedProbe);
          const sample = finalId
            ? stillSampleForSource(
                { ...state, sourcesById: sourcesSnapshot, samplesById: samplesSnapshot },
                finalId,
              )
            : null;
          if (sample) {
            samplesSnapshot = { ...samplesSnapshot, [sample.id]: sample };
            outcome.sampled += 1;
            onProjectChange((current) =>
              current.samplesById[sample.id]
                ? current
                : {
                    ...current,
                    samplesById: { ...current.samplesById, [sample.id]: sample },
                  },
            );
          }
        }
      } catch (probeError) {
        outcome.failed += 1;
        onProjectChange((current) => ({
          ...current,
          sourcesById: current.sourcesById[record.id]
            ? {
                ...current.sourcesById,
                [record.id]: {
                  ...current.sourcesById[record.id],
                  state: "error",
                  errorCode: mediaErrorCode(probeError),
                  errorDetail: String(probeError),
                },
              }
            : current.sourcesById,
        }));
      } finally {
        input.onBusyDelta?.(-1);
      }
    }),
  );
  return outcome;
}
