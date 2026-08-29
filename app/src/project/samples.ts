import type { ProjectState, Sample, Source } from "./types";

/**
 * Sample selectors and shared Source-error patches used by Media/Samples
 * pages. Pure transforms so they plug directly into `onProjectChange`.
 */

/** Samples with `included: true`, sorted by `order` (the analysis input set). */
export function includedSamples(state: ProjectState): Sample[] {
  return Object.values(state.samplesById)
    .filter((sample) => sample.included)
    .sort((a, b) => a.order - b.order);
}

/** Limit an included Sample list to a one-shot selection passed into Analyze. */
export function selectedAnalysisSamples(
  samples: Sample[],
  selectedIds?: readonly string[] | null,
): Sample[] {
  if (!selectedIds?.length) return samples;
  const selected = new Set(selectedIds);
  return samples.filter((sample) => selected.has(sample.id));
}

/** Global historical-result visibility: excluded Samples are hidden by default. */
export function hiddenResultSampleIds(
  samplesById: ProjectState["samplesById"],
  locallyHidden: ReadonlySet<string>,
  showExcluded: boolean,
): Set<string> {
  const hidden = new Set(locallyHidden);
  if (!showExcluded) {
    for (const sample of Object.values(samplesById)) {
      if (!sample.included) hidden.add(sample.id);
    }
  }
  return hidden;
}

export function resultSampleIsVisible(
  samplesById: ProjectState["samplesById"],
  sampleId: string | null,
  showExcluded: boolean,
): boolean {
  if (showExcluded || sampleId == null) return true;
  return samplesById[sampleId]?.included !== false;
}

/**
 * Returns a NEW ProjectState with the given Source patched to `state: "error"`
 * plus the provided errorCode/errorDetail. Missing source returns the input
 * state unchanged.
 */
export function applySourceError(
  state: ProjectState,
  sourceId: string,
  error: { code: string; detail: string },
): ProjectState {
  const source = state.sourcesById[sourceId];
  if (!source) return state;
  return {
    ...state,
    sourcesById: {
      ...state.sourcesById,
      [sourceId]: {
        ...source,
        state: "error",
        errorCode: error.code,
        errorDetail: error.detail,
      },
    },
  };
}

/**
 * Returns a NEW ProjectState with the given Source marked `state: "missing"`
 * (on top of the applySourceError patch). Missing source, or one already
 * marked missing, returns the input state unchanged.
 */
export function applySourceMissing(
  state: ProjectState,
  sourceId: string,
  error: { code: string; detail: string },
): ProjectState {
  const source = state.sourcesById[sourceId];
  if (!source || source.state === "missing") return state;
  const next = applySourceError(state, sourceId, error);
  const patched = next.sourcesById[sourceId];
  return {
    ...next,
    sourcesById: {
      ...next.sourcesById,
      [sourceId]: { ...patched, state: "missing" },
    },
  };
}

/** Returns a NEW ProjectState with the Sample inserted (replaces same id). */
export function withSample(state: ProjectState, sample: Sample): ProjectState {
  return { ...state, samplesById: { ...state.samplesById, [sample.id]: sample } };
}

/** Returns a NEW ProjectState without the given Sample. */
export function withoutSample(state: ProjectState, sampleId: string): ProjectState {
  const samplesById = { ...state.samplesById };
  delete samplesById[sampleId];
  return { ...state, samplesById };
}

/** Next `order` value for a new Sample: max existing order + 1 (0 when empty). */
export function nextSampleOrder(samplesById: Record<string, Sample>): number {
  return Math.max(-1, ...Object.values(samplesById).map((sample) => sample.order)) + 1;
}

/**
 * One Sample builder for both MediaPage (manual frame pick) and VerifyPage
 * (add-from-review): fills the id, source linkage, inclusion, time base (from
 * the Source's stream list) and empty tags. Label and pts semantics stay at
 * the call site — the two pages drift there on purpose.
 */
export function buildFrameSample(fields: {
  source: Source;
  order: number;
  label: string;
  streamIndex: number | null;
  frameIndex: number | null;
  pts?: number | null;
  bestEffortTimestamp?: number | null;
  timestampSeconds?: number | null;
  originRunId?: string | null;
}): Sample {
  const { source, order, label, streamIndex, frameIndex } = fields;
  const stream = source.videoStreams.find((item) => item.index === streamIndex);
  return {
    id: `smp_${crypto.randomUUID()}`,
    sourceId: source.id,
    sourceFingerprint: source.fingerprint ?? null,
    label,
    included: true,
    order,
    frameIndex,
    streamIndex,
    pts: fields.pts ?? null,
    bestEffortTimestamp: fields.bestEffortTimestamp ?? null,
    timeBaseNum: stream?.timeBaseNum ?? null,
    timeBaseDen: stream?.timeBaseDen ?? null,
    timestampSeconds: fields.timestampSeconds ?? null,
    tags: [],
    ...(fields.originRunId !== undefined ? { originRunId: fields.originRunId } : {}),
  };
}
