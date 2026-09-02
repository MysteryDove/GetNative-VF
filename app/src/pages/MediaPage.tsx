import { useCallback, useEffect, useMemo, useState, type KeyboardEvent } from "react";
import {
  AlertTriangle,
  FileImage,
  Film,
  FolderPlus,
  ImagePlus,
  LoaderCircle,
  Plus,
  RefreshCw,
  Trash2,
  ZoomIn,
  ZoomOut,
} from "lucide-react";
import type { Translator } from "../i18n";
import {
  getMediaCapabilities,
  pickMediaFiles,
  probeMedia,
  type MediaCapabilities,
} from "../media/service";
import type { ProjectState, Sample, Source } from "../project/types";
import { dispatchFrameBrowserKey, findDuplicateSampleId } from "../media/frameBrowser";
import {
  applyRelinkedProbe,
  cancelSourceImport,
  fileName,
  importMediaPaths,
} from "../media/importSources";
import { useFileDrop } from "../media/useFileDrop";
import { dimensionText, formatSeconds } from "../media/format";
import {
  mediaViewState,
  previewMessage,
  sourceSummary,
  videoSampleLabel,
} from "../media/helpers";
import {
  applySourceError,
  applySourceMissing,
  buildFrameSample,
  nextSampleOrder,
  withSample,
  withoutSample,
} from "../project/samples";
import { useIndexProgress, useMediaPreview } from "../hooks/useMediaPreview";
import { VideoFrameBrowser } from "../components/VideoFrameBrowser";

type ProjectUpdater = (updater: (state: ProjectState) => ProjectState) => void;

export function MediaPage({
  t,
  state,
  onProjectChange,
  active = true,
}: {
  t: Translator;
  state: ProjectState;
  onProjectChange: ProjectUpdater;
  active?: boolean;
}) {
  const sources = useMemo(() => Object.values(state.sourcesById), [state.sourcesById]);
  const persistedSelection = mediaViewState(state).selectedSourceId;
  const [selectedSourceId, setSelectedSourceId] = useState<string | null>(
    persistedSelection && state.sourcesById[persistedSelection]
      ? persistedSelection
      : sources[0]?.id ?? null,
  );
  const [mediaCapabilities, setMediaCapabilities] = useState<MediaCapabilities | null>(null);
  const [importing, setImporting] = useState(0);
  const [indexProgress, reportIndexProgress] = useIndexProgress(state.sourcesById);
  const [error, setError] = useState("");
  // Transient dedup popup: set only when an add is blocked by the dedup rule.
  const [dedupPopupId, setDedupPopupId] = useState<number | null>(null);

  const selectedSource = selectedSourceId ? state.sourcesById[selectedSourceId] : null;
  const sampleCountBySourceId = useMemo(() => {
    const counts: Record<string, number> = {};
    for (const sample of Object.values(state.samplesById)) {
      counts[sample.sourceId] = (counts[sample.sourceId] ?? 0) + 1;
    }
    return counts;
  }, [state.samplesById]);
  const sourceSamples = useMemo(
    () =>
      Object.values(state.samplesById)
        .filter((sample) => sample.sourceId === selectedSourceId)
        .sort((a, b) => a.order - b.order),
    [selectedSourceId, state.samplesById],
  );

  const selectSource = useCallback(
    (sourceId: string) => {
      setSelectedSourceId(sourceId);
      onProjectChange((current) => ({
        ...current,
        uiStateByRoute: {
          ...current.uiStateByRoute,
          media: { ...mediaViewState(current), selectedSourceId: sourceId },
        },
      }));
    },
    [onProjectChange],
  );

  const markSourceChanged = useCallback(
    (sourceId: string, detail: string) => {
      onProjectChange((current) =>
        applySourceError(current, sourceId, {
          code: "media_fingerprint_mismatch",
          detail,
        }),
      );
    },
    [onProjectChange],
  );

  const markSourceMissing = useCallback(
    (sourceId: string, detail: string) => {
      onProjectChange((current) =>
        applySourceMissing(current, sourceId, { code: "media_missing", detail }),
      );
    },
    [onProjectChange],
  );

  const {
    previewUrl,
    thumbnailUrls,
    previewBusy,
    previewDecoder,
    indexBusy,
    frameWindow,
    frameInput,
    setFrameInput,
    timeInput,
    setTimeInput,
    scrubFrame,
    zoom,
    setZoom,
    pixelPosition,
    setPixelPosition,
    selectVideoFrame,
    scrubVideoFrame,
  } = useMediaPreview({
    selectedSource,
    active,
    videoDecodeAvailable: mediaCapabilities?.video_decode_available,
    setError,
    onSourceChanged: markSourceChanged,
    onSourceMissing: markSourceMissing,
  });

  const importPaths = useCallback(
    async (paths: string[]) => {
      if (state.project.readOnly) return;
      setError("");
      await importMediaPaths({
        paths,
        state,
        onProjectChange,
        onPending: (id) => selectSource(id),
        onBusyDelta: (delta) => setImporting((count) => Math.max(0, count + delta)),
        onIndexProgress: reportIndexProgress,
      });
    },
    [onProjectChange, reportIndexProgress, selectSource, state],
  );

  useEffect(() => {
    if (selectedSourceId && state.sourcesById[selectedSourceId]) return;
    setSelectedSourceId(sources[0]?.id ?? null);
  }, [selectedSourceId, sources, state.sourcesById]);

  // The dedup popup never survives a frame/source switch.
  const selectedFrameIndex = frameWindow?.selected.frame_index ?? null;
  useEffect(() => {
    setDedupPopupId(null);
  }, [selectedSourceId, selectedFrameIndex]);

  useEffect(() => {
    if (dedupPopupId == null) return;
    const timer = window.setTimeout(() => setDedupPopupId(null), 3200);
    return () => window.clearTimeout(timer);
  }, [dedupPopupId]);

  useEffect(() => {
    getMediaCapabilities().then(setMediaCapabilities).catch((reason) => setError(String(reason)));
  }, []);

  const dropActive = useFileDrop((paths) => void importPaths(paths), active);

  async function pickFiles() {
    try {
      await importPaths(await pickMediaFiles());
    } catch (reason) {
      setError(String(reason));
    }
  }

  async function relinkSource(source: Source) {
    try {
      const [path] = await pickMediaFiles();
      if (!path) return;
      const probe = await probeMedia(path);
      if (
        source.fingerprint &&
        probe.fingerprint !== source.fingerprint &&
        !window.confirm(t("media.relinkChangedConfirm"))
      ) {
        return;
      }
      await applyRelinkedProbe({
        sourceId: source.id,
        probe,
        label: source.label,
        onProjectChange,
        onIndexProgress: reportIndexProgress,
      });
    } catch (reason) {
      setError(String(reason));
    }
  }

  function removeSource(source: Source) {
    if (state.project.readOnly) return;
    const sampleCount = sampleCountBySourceId[source.id] ?? 0;
    if (sampleCount > 0) {
      if (!window.confirm(t("media.removeSourceWithSamplesConfirm", { count: sampleCount }))) return;
      if (!window.confirm(t("media.removeSourceWithSamplesConfirmAgain", { count: sampleCount }))) return;
    }
    void cancelSourceImport(source.id).catch(() => undefined);
    onProjectChange((current) => {
      const sourcesById = { ...current.sourcesById };
      delete sourcesById[source.id];
      const samplesById = { ...current.samplesById };
      for (const sample of Object.values(samplesById)) {
        if (sample.sourceId === source.id) delete samplesById[sample.id];
      }
      return { ...current, sourcesById, samplesById };
    });
    if (selectedSourceId === source.id) {
      const next = sources.find((item) => item.id !== source.id);
      setSelectedSourceId(next?.id ?? null);
    }
  }

  function handleFrameBrowserKeyDown(
    event: KeyboardEvent,
    source: Source,
  ) {
    if (source.kind !== "video") return;
    dispatchFrameBrowserKey(event, {
      frameWindow,
      previewBusy,
      select: (target, frameIndex) => void selectVideoFrame(source, target, frameIndex),
    });
  }

  function addSample() {
    if (!selectedSource || selectedSource.state !== "ready") return;
    if (selectedSource.kind === "animated") return;
    const selectedFrame = selectedSource.kind === "video" ? frameWindow?.selected : null;
    if (selectedSource.kind === "video" && !selectedFrame) return;
    const duplicateId = findDuplicateSampleId(Object.values(state.samplesById), {
      sourceId: selectedSource.id,
      kind: selectedSource.kind,
      streamIndex: selectedSource.kind === "video" ? selectedSource.selectedStreamIndex : null,
      frameIndex: selectedFrame?.frame_index ?? null,
    });
    if (duplicateId && !window.confirm(t("media.duplicateSampleConfirm"))) {
      setDedupPopupId((id) => (id ?? 0) + 1);
      return;
    }
    const sample = buildFrameSample({
      source: selectedSource,
      order: nextSampleOrder(state.samplesById),
      label:
        selectedSource.kind === "video"
          ? videoSampleLabel(selectedSource, selectedFrame?.frame_index, t)
          : t("media.kind.still"),
      streamIndex: selectedSource.kind === "video" ? selectedSource.selectedStreamIndex ?? null : null,
      frameIndex: selectedFrame?.frame_index ?? null,
      pts: selectedFrame?.pts ?? null,
      bestEffortTimestamp: selectedFrame?.best_effort_timestamp ?? null,
      timestampSeconds: selectedFrame?.timestamp_seconds ?? null,
    });
    onProjectChange((current) => withSample(current, sample));
  }

  function removeSample(sampleId: string) {
    onProjectChange((current) => withoutSample(current, sampleId));
  }

  function setSampleIncluded(sampleId: string, included: boolean) {
    if (state.project.readOnly) return;
    onProjectChange((current) => {
      const sample = current.samplesById[sampleId];
      if (!sample || sample.included === included) return current;
      return {
        ...current,
        samplesById: { ...current.samplesById, [sampleId]: { ...sample, included } },
      };
    });
  }

  function seekToSample(sample: Sample) {
    if (!selectedSource || selectedSource.kind !== "video" || sample.frameIndex == null) return;
    void selectVideoFrame(selectedSource, "frame", sample.frameIndex);
  }

  return (
    <div className={`page-panel media-page ${dropActive ? "drop-active" : ""}`}>
      <div className="page-header media-header">
        <div>
          <h2>{t("media.title")}</h2>
          <span>{t("media.subtitle")}</span>
        </div>
        <div className="media-header-actions">
          <span className={`media-backend ${mediaCapabilities?.video_decode_available ? "ready" : "limited"}`}>
            {mediaCapabilities?.video_decode_available
              ? t("media.videoReady")
              : t("media.videoUnavailable")}
          </span>
          <button
            className="secondary-button"
            type="button"
            onClick={() => void pickFiles()}
            disabled={state.project.readOnly}
          >
            {importing ? <LoaderCircle className="spin" size={15} /> : <FolderPlus size={15} />}
            {t("media.import")}
          </button>
        </div>
      </div>

      {error ? (
        <div className="media-error" role="alert">
          <AlertTriangle size={15} />
          <span>{error}</span>
        </div>
      ) : null}

      <div className="media-workspace">
        <aside className="source-pane">
          <div className="pane-heading">
            <strong>{t("media.sources")}</strong>
            <span>{sources.length}</span>
          </div>
          {sources.length ? (
            <div className="source-list">
              {sources.map((source) => {
                const addedCount = sampleCountBySourceId[source.id] ?? 0;
                return (
                  <div
                    className={`source-row ${source.id === selectedSourceId ? "selected" : ""}`}
                    key={source.id}
                  >
                    <button
                      className="source-row-main"
                      type="button"
                      onClick={() => selectSource(source.id)}
                    >
                      <span className={`source-kind ${source.state}`}>
                        {source.kind === "still" ? <FileImage size={16} /> : <Film size={16} />}
                      </span>
                      <span className="source-copy">
                        <strong>{source.label ?? fileName(source.path)}</strong>
                        <small>
                          {sourceSummary(source, t)}
                          {source.state === "probing" && indexProgress[source.id] != null
                            ? ` · ${t("media.indexedFrames", { count: indexProgress[source.id] })}`
                            : ""}
                          {` · ${t("media.addedFrameCount", { count: addedCount })}`}
                        </small>
                      </span>
                      {source.state === "probing" ? <LoaderCircle className="spin" size={14} /> : null}
                    </button>
                    <button
                      className="icon-button"
                      type="button"
                      title={t("media.removeSource")}
                      aria-label={t("media.removeSource")}
                      disabled={state.project.readOnly}
                      onClick={(event) => {
                        event.stopPropagation();
                        removeSource(source);
                      }}
                    >
                      <Trash2 size={14} />
                    </button>
                  </div>
                );
              })}
            </div>
          ) : (
            <button className="source-empty" type="button" onClick={() => void pickFiles()}>
              <FolderPlus size={22} />
              <strong>{t("media.dropTitle")}</strong>
              <span>{t("media.dropBody")}</span>
            </button>
          )}
        </aside>

        <main className="media-inspector">
          {selectedSource ? (
            <>
              <div className="inspector-toolbar">
                <div>
                  <strong>{selectedSource.label ?? fileName(selectedSource.path)}</strong>
                  <span title={selectedSource.path}>{selectedSource.path}</span>
                </div>
                {(selectedSource.state === "missing" || selectedSource.state === "error") ? (
                  <div className="inspector-actions">
                    <button
                      className="icon-button reveal-button"
                      type="button"
                      title={t("media.relink")}
                      aria-label={t("media.relink")}
                      onClick={() => void relinkSource(selectedSource)}
                    >
                      <RefreshCw size={14} />
                    </button>
                  </div>
                ) : null}
              </div>

              <div
                className="media-viewport"
                tabIndex={selectedSource.kind === "video" && frameWindow ? 0 : undefined}
                aria-label={selectedSource.kind === "video" ? t("media.frameWindow") : undefined}
                onKeyDown={(event) => handleFrameBrowserKeyDown(event, selectedSource)}
                onMouseMove={(event) => {
                  if (!selectedSource.width || !selectedSource.height) return;
                  const rect = event.currentTarget.getBoundingClientRect();
                  const x = Math.max(0, Math.min(selectedSource.width - 1, Math.floor(((event.clientX - rect.left) / rect.width) * selectedSource.width)));
                  const y = Math.max(0, Math.min(selectedSource.height - 1, Math.floor(((event.clientY - rect.top) / rect.height) * selectedSource.height)));
                  setPixelPosition(`${x}, ${y}`);
                }}
                onMouseLeave={() => setPixelPosition(null)}
              >
                {previewUrl ? (
                  <img
                    src={previewUrl}
                    alt={selectedSource.label ?? fileName(selectedSource.path)}
                    draggable={false}
                    style={{ transform: `scale(${zoom})` }}
                  />
                ) : (
                  <div className="viewport-empty">
                    {previewBusy ? <LoaderCircle className="spin" size={24} /> : <Film size={24} />}
                    <span>{previewMessage(selectedSource, mediaCapabilities, t)}</span>
                  </div>
                )}
                {previewBusy && previewUrl ? <LoaderCircle className="viewport-spinner spin" size={22} /> : null}
                {previewUrl ? (
                  <div className="viewport-readout">
                    <span>{pixelPosition ?? t("media.pixelPosition")}</span>
                    <button type="button" onClick={() => setZoom((value) => Math.max(0.5, value - 0.25))} aria-label={t("media.zoomOut")}>
                      <ZoomOut size={14} />
                    </button>
                    <span>{Math.round(zoom * 100)}%</span>
                    <button type="button" onClick={() => setZoom((value) => Math.min(4, value + 0.25))} aria-label={t("media.zoomIn")}>
                      <ZoomIn size={14} />
                    </button>
                  </div>
                ) : null}
              </div>

              {selectedSource.kind === "video" ? (
                <VideoFrameBrowser
                  t={t}
                  source={selectedSource}
                  frameWindow={frameWindow}
                  previewBusy={previewBusy}
                  indexBusy={indexBusy}
                  frameInput={frameInput}
                  timeInput={timeInput}
                  scrubFrame={scrubFrame}
                  thumbnailUrls={thumbnailUrls}
                  sourceSamples={sourceSamples}
                  onFrameInputChange={setFrameInput}
                  onTimeInputChange={setTimeInput}
                  onScrubFrameChange={(frameIndex) => scrubVideoFrame(selectedSource, frameIndex)}
                  selectVideoFrame={selectVideoFrame}
                  onKeyDown={(event) => handleFrameBrowserKeyDown(event, selectedSource)}
                />
              ) : null}

              <div className="source-metadata">
                <span>{t("media.dimensions")} <strong>{dimensionText(selectedSource?.width, selectedSource?.height)}</strong></span>
                <span>{t("media.type")} <strong>{t(`media.kind.${selectedSource.kind}`)}</strong></span>
                <span>
                  {t(selectedSource.kind === "video" ? "media.previewDecoder" : "media.decoder")} {" "}
                  <strong>
                    {selectedSource.kind === "video"
                      ? (previewDecoder ?? selectedSource.decoder ?? "-")
                      : (selectedSource.decoder ?? "-")}
                  </strong>
                </span>
                {selectedSource.kind === "video" ? <span>{t("media.duration")} <strong>{formatSeconds(selectedSource.durationSeconds)}</strong></span> : null}
              </div>
            </>
          ) : (
            <div className="workspace-empty">
              <FolderPlus size={28} />
              <strong>{t("media.dropTitle")}</strong>
              <span>{t("media.dropBody")}</span>
            </div>
          )}
        </main>

        <aside className="selection-pane">
          <div className="pane-heading">
            <strong>{t("media.selectedSamples")}</strong>
            <span>{sourceSamples.length}</span>
          </div>
          <div className="sample-add-actions">
            <button
              className="secondary-button primary-command"
              type="button"
              disabled={!selectedSource || selectedSource.state !== "ready" || selectedSource.kind === "animated" || (selectedSource.kind === "video" && !frameWindow) || state.project.readOnly}
              onClick={() => addSample()}
            >
              {selectedSource?.kind === "video" ? <Plus size={15} /> : <ImagePlus size={15} />}
              {selectedSource?.kind === "video" ? t("media.addCurrentFrame") : t("media.addImage")}
            </button>
            {dedupPopupId != null ? (
              <p className="dedup-popup" role="status">
                {t("media.alreadySelected")}
              </p>
            ) : null}
          </div>
          <div className="selected-sample-list">
            {sourceSamples.map((sample) => {
              const currentFrame = frameWindow?.selected.frame_index;
              const isCurrent = sample.frameIndex != null && sample.frameIndex === currentFrame;
              return (
                <div
                  className={`selected-sample-row${sample.included ? "" : " excluded"}${isCurrent ? " current" : ""}`}
                  key={sample.id}
                  onClick={() => seekToSample(sample)}
                >
                  <strong>
                    {sample.frameIndex != null ? `#${sample.frameIndex}` : t("media.kind.still")}
                  </strong>
                  <label
                    className="include-switch"
                    title={sample.included ? t("samples.included") : t("samples.excluded")}
                    onClick={(event) => event.stopPropagation()}
                  >
                    <input
                      type="checkbox"
                      checked={sample.included}
                      disabled={state.project.readOnly}
                      aria-label={sample.included ? t("samples.included") : t("samples.excluded")}
                      onChange={(event) => setSampleIncluded(sample.id, event.target.checked)}
                    />
                    <span className="include-switch-track" />
                  </label>
                  <button
                    className="icon-button"
                    type="button"
                    title={t("samples.remove")}
                    aria-label={t("samples.remove")}
                    disabled={state.project.readOnly}
                    onClick={(event) => {
                      event.stopPropagation();
                      removeSample(sample.id);
                    }}
                  >
                    <Trash2 size={14} />
                  </button>
                </div>
              );
            })}
            {!sourceSamples.length ? <p className="selection-empty">{t("media.noSelectedSamples")}</p> : null}
          </div>
        </aside>
      </div>

      {dropActive ? (
        <div className="drop-overlay">
          <FolderPlus size={30} />
          <strong>{t("media.dropNow")}</strong>
        </div>
      ) : null}
    </div>
  );
}
