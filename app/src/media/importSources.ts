import type { ProjectState, Sample, Source } from "../project/types";
import { probeMedia, type MediaProbeResult } from "./service";
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

export async function importMediaPaths(input: {
  paths: string[];
  state: ProjectState;
  onProjectChange: ProjectUpdater;
  /** Create included Samples for still images (Samples page flow). */
  createSamplesForStills?: boolean;
  /** Called synchronously with the first provisional Source id (e.g. to select it). */
  onPending?: (provisionalId: string) => void;
  onBusyDelta?: (delta: number) => void;
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
        if (probe.kind === "video") outcome.videos += 1;
        if (input.createSamplesForStills && probe.kind === "still" && probe.state === "ready") {
          const finalId = finalSourceIdFor(sourcesSnapshot, record.id, probe);
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
          sourcesById: {
            ...current.sourcesById,
            [record.id]: {
              ...current.sourcesById[record.id],
              state: "error",
              errorCode: "media_probe_error",
              errorDetail: String(probeError),
            },
          },
        }));
      } finally {
        input.onBusyDelta?.(-1);
      }
    }),
  );
  return outcome;
}
