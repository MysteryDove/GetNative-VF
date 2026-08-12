import { useCallback, useEffect, useMemo, useRef, useState, type KeyboardEvent } from "react";
import {
  AlertTriangle,
  ChevronLeft,
  ChevronRight,
  ChevronsLeft,
  ChevronsRight,
  FileImage,
  Film,
  FolderPlus,
  ImagePlus,
  LoaderCircle,
  LocateFixed,
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
  requestFrameWindow,
  requestMediaPreview,
  requestMediaPreviewBatch,
  type FrameWindowTarget,
  type MediaCapabilities,
  type MediaFrameWindow,
} from "../media/service";
import type { ProjectState, Sample, Source } from "../project/types";
import { findDuplicateSampleId, frameStepFromKeyboard } from "../media/frameBrowser";
import {
  beginSourceMediaIndex,
  cancelSourceImport,
  fileName,
  importMediaPaths,
  indexedVideoProbe,
  sourceFromProbe,
} from "../media/importSources";
import { useFileDrop } from "../media/useFileDrop";
import { dimensionText, formatSeconds } from "../media/format";
import { applySourceError } from "../project/samples";

type ProjectUpdater = (updater: (state: ProjectState) => ProjectState) => void;

export function MediaPage({
  t,
  state,
  onProjectChange,
}: {
  t: Translator;
  state: ProjectState;
  onProjectChange: ProjectUpdater;
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
  const [indexProgress, setIndexProgress] = useState<Record<string, number>>({});
  const [error, setError] = useState("");
  const [frameWindowState, setFrameWindowState] = useState<{
    sourceKey: string;
    window: MediaFrameWindow;
  } | null>(null);
  const [previewUrl, setPreviewUrl] = useState<string | null>(null);
  const [thumbnailUrls, setThumbnailUrls] = useState<Record<number, string>>({});
  const [previewBusy, setPreviewBusy] = useState(false);
  const [indexBusy, setIndexBusy] = useState(false);
  const [frameInput, setFrameInput] = useState("0");
  const [timeInput, setTimeInput] = useState("0.000");
  const [scrubFrame, setScrubFrame] = useState(0);
  const [zoom, setZoom] = useState(1);
  const [pixelPosition, setPixelPosition] = useState<string | null>(null);
  const previewRequestId = useRef(0);
  const sourceSessionId = useRef(0);
  const activeMediaTask = useRef<{ cancel: () => Promise<void> } | null>(null);
  /** Tracks which source/stream the displayed preview belongs to. */
  const previewKeyRef = useRef("");
  const previewUrlRef = useRef<string | null>(null);
  const thumbnailUrlsRef = useRef<Record<number, string>>({});

  /** Atomic preview swap: the old image stays visible until its replacement exists. */
  const swapPreviewUrl = useCallback((next: string | null) => {
    const previous = previewUrlRef.current;
    previewUrlRef.current = next;
    setPreviewUrl(next);
    if (previous && previous !== next) URL.revokeObjectURL(previous);
  }, []);

  const swapThumbnailUrls = useCallback((next: Record<number, string>) => {
    for (const url of Object.values(thumbnailUrlsRef.current)) URL.revokeObjectURL(url);
    thumbnailUrlsRef.current = next;
    setThumbnailUrls(next);
  }, []);

  const selectedSource = selectedSourceId ? state.sourcesById[selectedSourceId] : null;
  const frameWindow =
    selectedSource && frameWindowState?.sourceKey === sourceStreamKey(selectedSource)
      ? frameWindowState.window
      : null;
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
      onProjectChange((current) => {
        const source = current.sourcesById[sourceId];
        if (!source || source.state === "missing") return current;
        const next = applySourceError(current, sourceId, {
          code: "media_missing",
          detail,
        });
        const patched = next.sourcesById[sourceId];
        return {
          ...next,
          sourcesById: {
            ...next.sourcesById,
            [sourceId]: { ...patched, state: "missing" },
          },
        };
      });
    },
    [onProjectChange],
  );

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
        onIndexProgress: (id, frames) => {
          setIndexProgress((current) => ({ ...current, [id]: frames }));
        },
      });
    },
    [onProjectChange, selectSource, state],
  );

  useEffect(() => {
    if (selectedSourceId && state.sourcesById[selectedSourceId]) return;
    setSelectedSourceId(sources[0]?.id ?? null);
  }, [selectedSourceId, sources, state.sourcesById]);

  useEffect(() => {
    getMediaCapabilities().then(setMediaCapabilities).catch((reason) => setError(String(reason)));
  }, []);

  const dropActive = useFileDrop((paths) => void importPaths(paths));

  const showPreview = useCallback(async (
    request: Parameters<typeof requestMediaPreview>[0],
    sourceId?: string,
  ) => {
    const requestId = ++previewRequestId.current;
    await activeMediaTask.current?.cancel().catch(() => undefined);
    const task = requestMediaPreview(request);
    activeMediaTask.current = task;
    setPreviewBusy(true);
    try {
      const nextUrl = await task.promise;
      if (previewRequestId.current !== requestId) {
        URL.revokeObjectURL(nextUrl);
        return;
      }
      swapPreviewUrl(nextUrl);
      setError("");
    } catch (reason) {
      // Keep the last good preview on screen; the error banner carries the detail.
      if (previewRequestId.current === requestId) {
        const detail = String(reason);
        setError(detail);
        previewKeyRef.current = "";
        if (sourceId && isFingerprintError(detail)) {
          markSourceChanged(sourceId, detail);
        } else if (sourceId && detail.includes("media_missing")) {
          markSourceMissing(sourceId, detail);
        }
      }
    } finally {
      if (previewRequestId.current === requestId) setPreviewBusy(false);
    }
  }, [markSourceChanged, markSourceMissing, swapPreviewUrl]);

  const selectVideoFrame = useCallback(
    async (
      source: Source,
      target: FrameWindowTarget,
      frameIndex?: number,
      timestampSeconds?: number,
    ) => {
      const streamIndex = source.selectedStreamIndex ?? source.videoStreams[0]?.index;
      if (streamIndex === undefined) return;
      const requestId = ++previewRequestId.current;
      await activeMediaTask.current?.cancel().catch(() => undefined);
      setPreviewBusy(true);
      try {
        const windowTask = requestFrameWindow({
          path: source.path,
          fingerprint: source.fingerprint,
          streamIndex,
          target,
          frameIndex,
          timestampSeconds,
          windowRadius: 12,
        });
        activeMediaTask.current = windowTask;
        const window = await windowTask.promise;
        if (previewRequestId.current !== requestId) return;
        const batchTask = requestMediaPreviewBatch({
          path: source.path,
          fingerprint: source.fingerprint,
          streamIndex,
          frames: [
            { itemId: "main", frameIndex: window.selected.frame_index, maxDimension: 1600 },
            ...window.frames.slice(0, 25).map((frame) => ({
              itemId: `thumb-${frame.frame_index}`,
              frameIndex: frame.frame_index,
              maxDimension: 160,
            })),
          ],
        });
        activeMediaTask.current = batchTask;
        const batch = await batchTask.promise;
        if (previewRequestId.current !== requestId) {
          for (const asset of batch.assets) URL.revokeObjectURL(asset.url);
          return;
        }
        const main = batch.assets.find((asset) => asset.itemId === "main");
        if (!main) throw new Error("media_decode_error: preview batch omitted the main frame");
        const thumbnails = Object.fromEntries(
          batch.assets
            .filter((asset) => asset.itemId.startsWith("thumb-"))
            .map((asset) => [asset.frameIndex, asset.url]),
        );
        setFrameWindowState({ sourceKey: sourceStreamKey(source), window });
        setFrameInput(String(window.selected.frame_index));
        setScrubFrame(window.selected.frame_index);
        setTimeInput((window.selected.timestamp_seconds ?? 0).toFixed(3));
        swapPreviewUrl(main.url);
        swapThumbnailUrls(thumbnails);
        setError("");
      } catch (reason) {
        if (previewRequestId.current === requestId) {
          const detail = String(reason);
          setError(detail);
          if (isFingerprintError(detail)) {
            markSourceChanged(source.id, detail);
          }
        }
      } finally {
        if (previewRequestId.current === requestId) setPreviewBusy(false);
      }
    },
    [markSourceChanged, swapPreviewUrl, swapThumbnailUrls],
  );

  const initializeVideoSource = useCallback(
    async (source: Source, sessionId: number) => {
      const streamIndex = source.selectedStreamIndex ?? source.videoStreams[0]?.index;
      if (streamIndex === undefined) return;
      setTimeInput("0.000");
      setIndexBusy(true);
      try {
        await selectVideoFrame(source, "frame", 0);
      } catch (reason) {
        if (sourceSessionId.current !== sessionId) return;
        const detail = String(reason);
        setError(detail);
        if (isFingerprintError(detail)) {
          markSourceChanged(source.id, detail);
        }
      } finally {
        if (sourceSessionId.current === sessionId) setIndexBusy(false);
      }
    },
    [markSourceChanged, selectVideoFrame],
  );

  const selectedPreviewKey =
    selectedSource && selectedSource.state === "ready"
      ? selectedSource.kind === "video"
        ? `video:${sourceStreamKey(selectedSource)}`
        : `still:${selectedSource.id}:${selectedSource.fingerprint ?? ""}`
      : "";

  useEffect(() => {
    const sessionId = ++sourceSessionId.current;
    // Same source already on screen (e.g. autosave rebuilt state objects with
    // fresh identities): keep the preview instead of reloading it.
    if (previewKeyRef.current === selectedPreviewKey && previewUrlRef.current) return;
    previewKeyRef.current = selectedPreviewKey;
    setFrameWindowState(null);
    setIndexBusy(false);
    setZoom(1);
    setPixelPosition(null);
    previewRequestId.current += 1;
    void activeMediaTask.current?.cancel().catch(() => undefined);
    swapPreviewUrl(null);
    swapThumbnailUrls({});
    if (!selectedSource || !selectedPreviewKey) return;
    if (selectedSource.kind === "video") {
      if (mediaCapabilities?.video_decode_available) {
        void initializeVideoSource(selectedSource, sessionId);
      }
    } else {
      void showPreview(
        { path: selectedSource.path, fingerprint: selectedSource.fingerprint },
        selectedSource.id,
      );
    }
    // selectedSource is read from the render that produced selectedPreviewKey.
  }, [initializeVideoSource, mediaCapabilities?.video_decode_available, selectedPreviewKey, showPreview, swapPreviewUrl, swapThumbnailUrls]); // eslint-disable-line react-hooks/exhaustive-deps

  // Revoke the live object URL only on unmount; in-flight swaps own their URLs.
  useEffect(
    () => () => {
      previewRequestId.current += 1;
      void activeMediaTask.current?.cancel().catch(() => undefined);
      if (previewUrlRef.current) URL.revokeObjectURL(previewUrlRef.current);
      for (const url of Object.values(thumbnailUrlsRef.current)) URL.revokeObjectURL(url);
    },
    [],
  );

  async function pickFiles() {
    try {
      await importPaths(await pickMediaFiles());
    } catch (reason) {
      setError(String(reason));
    }
  }

  async function relinkSource(source: Source) {
    const [path] = await pickMediaFiles();
    if (!path) return;
    try {
      const probe = await probeMedia(path);
      if (
        source.fingerprint &&
        probe.fingerprint !== source.fingerprint &&
        !window.confirm(t("media.relinkChangedConfirm"))
      ) {
        return;
      }
      onProjectChange((current) => ({
        ...current,
        sourcesById: {
          ...current.sourcesById,
          [source.id]: sourceFromProbe(source.id, probe, source.label),
        },
      }));
      if (probe.kind === "video" && probe.state === "probing") {
        const task = beginSourceMediaIndex(
          source.id,
          { path: probe.path, fingerprint: probe.fingerprint },
          (progress) => setIndexProgress((current) => ({
            ...current,
            [source.id]: progress.completed,
          })),
        );
        const indexed = indexedVideoProbe(probe, await task.promise);
        onProjectChange((current) => current.sourcesById[source.id]
          ? {
              ...current,
              sourcesById: {
                ...current.sourcesById,
                [source.id]: sourceFromProbe(source.id, indexed, source.label),
              },
            }
          : current);
      }
    } catch (reason) {
      setError(String(reason));
    }
  }

  function removeSource(source: Source) {
    if (Object.values(state.samplesById).some((sample) => sample.sourceId === source.id)) return;
    void cancelSourceImport(source.id).catch(() => undefined);
    onProjectChange((current) => {
      const sourcesById = { ...current.sourcesById };
      delete sourcesById[source.id];
      return { ...current, sourcesById };
    });
    const next = sources.find((item) => item.id !== source.id);
    setSelectedSourceId(next?.id ?? null);
  }

  function showNearbyTime(source: Source) {
    const streamIndex = source.selectedStreamIndex ?? source.videoStreams[0]?.index;
    const requested = Number(timeInput);
    if (streamIndex === undefined || !Number.isFinite(requested) || requested < 0) return;
    const timestampSeconds = Math.min(requested, source.durationSeconds ?? requested);
    setTimeInput(timestampSeconds.toFixed(3));
    void selectVideoFrame(source, "timestamp", undefined, timestampSeconds);
  }

  function handleFrameBrowserKeyDown(
    event: KeyboardEvent,
    source: Source,
  ) {
    if (source.kind !== "video" || !frameWindow || previewBusy) return;
    const target = event.target as HTMLElement | null;
    if (target && (target.tagName === "INPUT" || target.tagName === "SELECT" || target.tagName === "TEXTAREA")) {
      return;
    }
    const action = frameStepFromKeyboard(event.key, { shiftKey: event.shiftKey });
    if (!action) return;
    event.preventDefault();
    const current = frameWindow.selected.frame_index;
    const max = Math.max(0, (frameWindow.total_frames ?? 1) - 1);
    switch (action.type) {
      case "previousFrame":
        if (current > 0) void selectVideoFrame(source, "frame", current - 1);
        break;
      case "nextFrame":
        if (current < max) void selectVideoFrame(source, "frame", current + 1);
        break;
      case "previousKeyframe":
        if (frameWindow.previous_keyframe != null) {
          void selectVideoFrame(source, "previousKeyframe", current);
        }
        break;
      case "nextKeyframe":
        if (frameWindow.next_keyframe != null) {
          void selectVideoFrame(source, "nextKeyframe", current);
        }
        break;
      case "firstFrame":
        void selectVideoFrame(source, "frame", 0);
        break;
      case "lastFrame":
        void selectVideoFrame(source, "frame", max);
        break;
      default:
        break;
    }
  }

  function addSample(andContinue: boolean) {
    if (!selectedSource || selectedSource.state !== "ready") return;
    if (selectedSource.kind === "animated") return;
    const selectedFrame = selectedSource.kind === "video" ? frameWindow?.selected : null;
    if (selectedSource.kind === "video" && !selectedFrame) return;
    const stream = selectedSource.videoStreams.find(
      (item) => item.index === selectedSource.selectedStreamIndex,
    );
    const duplicateId = findDuplicateSampleId(Object.values(state.samplesById), {
      sourceId: selectedSource.id,
      kind: selectedSource.kind,
      streamIndex: selectedSource.kind === "video" ? selectedSource.selectedStreamIndex : null,
      frameIndex: selectedFrame?.frame_index ?? null,
    });
    if (duplicateId && !window.confirm(t("media.duplicateSampleConfirm"))) {
      return;
    }
    const order = Math.max(-1, ...Object.values(state.samplesById).map((sample) => sample.order)) + 1;
    const sample: Sample = {
      id: `smp_${crypto.randomUUID()}`,
      sourceId: selectedSource.id,
      sourceFingerprint: selectedSource.fingerprint ?? null,
      label:
        selectedSource.kind === "video"
          ? videoSampleLabel(selectedSource, selectedFrame?.frame_index, t)
          : selectedSource.label ?? fileName(selectedSource.path),
      included: true,
      order,
      frameIndex: selectedFrame?.frame_index ?? null,
      streamIndex: selectedSource.kind === "video" ? selectedSource.selectedStreamIndex : null,
      pts: selectedFrame?.pts ?? null,
      bestEffortTimestamp: selectedFrame?.best_effort_timestamp ?? null,
      timeBaseNum: stream?.timeBaseNum ?? null,
      timeBaseDen: stream?.timeBaseDen ?? null,
      timestampSeconds: selectedFrame?.timestamp_seconds ?? null,
      tags: [],
    };
    onProjectChange((current) => ({
      ...current,
      samplesById: { ...current.samplesById, [sample.id]: sample },
    }));
    if (andContinue && selectedFrame) {
      void selectVideoFrame(selectedSource, "frame", selectedFrame.frame_index + 1);
    }
  }

  function removeSample(sampleId: string) {
    onProjectChange((current) => {
      const samplesById = { ...current.samplesById };
      delete samplesById[sampleId];
      return { ...current, samplesById };
    });
  }

  const currentFrame = frameWindow?.selected.frame_index ?? 0;
  const maxFrame = Math.max(0, (frameWindow?.total_frames ?? 1) - 1);
  const alreadySelected = Boolean(
    selectedSource &&
      Object.values(state.samplesById).some(
        (sample) =>
          sample.sourceId === selectedSource.id &&
          (selectedSource.kind !== "video" ||
            (sample.streamIndex === selectedSource.selectedStreamIndex &&
              sample.frameIndex === currentFrame)),
      ),
  );
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
              {sources.map((source) => (
                <button
                  className={`source-row ${source.id === selectedSourceId ? "selected" : ""}`}
                  type="button"
                  key={source.id}
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
                    </small>
                  </span>
                  {source.state === "probing" ? <LoaderCircle className="spin" size={14} /> : null}
                </button>
              ))}
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
                <div className="inspector-actions">
                  {(selectedSource.state === "missing" || selectedSource.state === "error") && (
                    <button
                      className="icon-button reveal-button"
                      type="button"
                      title={t("media.relink")}
                      aria-label={t("media.relink")}
                      onClick={() => void relinkSource(selectedSource)}
                    >
                      <RefreshCw size={14} />
                    </button>
                  )}
                  <button
                    className="icon-button"
                    type="button"
                    title={t("media.removeSource")}
                    aria-label={t("media.removeSource")}
                    disabled={sourceSamples.length > 0 || state.project.readOnly}
                    onClick={() => removeSource(selectedSource)}
                  >
                    <Trash2 size={14} />
                  </button>
                </div>
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
                <div
                  className="frame-browser"
                  tabIndex={frameWindow ? 0 : undefined}
                  onKeyDown={(event) => handleFrameBrowserKeyDown(event, selectedSource)}
                >
                  {frameWindow ? (
                    <>
                      <p className="frame-keyboard-help">{t("media.keyboardHelp")}</p>
                      <div className="frame-controls">
                        <button
                          className="icon-button"
                          type="button"
                          title={t("media.previousKeyframe")}
                          aria-label={t("media.previousKeyframe")}
                          disabled={previewBusy || !frameWindow.previous_keyframe}
                          onClick={() => void selectVideoFrame(selectedSource, "previousKeyframe", currentFrame)}
                        >
                          <ChevronsLeft size={14} />
                        </button>
                        <button
                          className="icon-button"
                          type="button"
                          title={t("media.previousFrame")}
                          aria-label={t("media.previousFrame")}
                          disabled={previewBusy || currentFrame <= 0}
                          onClick={() => void selectVideoFrame(selectedSource, "frame", currentFrame - 1)}
                        >
                          <ChevronLeft size={14} />
                        </button>
                        <label>
                          <span>{t("media.frameNumber")}</span>
                          <input value={frameInput} inputMode="numeric" onChange={(event) => setFrameInput(event.target.value)} onKeyDown={(event) => {
                            if (event.key === "Enter") void selectVideoFrame(selectedSource, "frame", Number(frameInput));
                          }} />
                        </label>
                        <button
                          className="icon-button"
                          type="button"
                          title={t("media.nextFrame")}
                          aria-label={t("media.nextFrame")}
                          disabled={previewBusy || currentFrame >= maxFrame}
                          onClick={() => void selectVideoFrame(selectedSource, "frame", currentFrame + 1)}
                        >
                          <ChevronRight size={14} />
                        </button>
                        <button
                          className="icon-button"
                          type="button"
                          title={t("media.nextKeyframe")}
                          aria-label={t("media.nextKeyframe")}
                          disabled={previewBusy || !frameWindow.next_keyframe}
                          onClick={() => void selectVideoFrame(selectedSource, "nextKeyframe", currentFrame)}
                        >
                          <ChevronsRight size={14} />
                        </button>
                        <label>
                          <span>{t("media.timestamp")}</span>
                          <input value={timeInput} inputMode="decimal" onChange={(event) => setTimeInput(event.target.value)} onKeyDown={(event) => {
                            if (event.key === "Enter") void selectVideoFrame(selectedSource, "timestamp", undefined, Number(timeInput));
                          }} />
                        </label>
                        <button className="icon-button reveal-button" type="button" title={t("media.goToTime")} aria-label={t("media.goToTime")} onClick={() => void selectVideoFrame(selectedSource, "timestamp", undefined, Number(timeInput))}>
                          <LocateFixed size={14} />
                        </button>
                      </div>
                      <input
                        className="frame-timeline"
                        type="range"
                        min={0}
                        max={maxFrame}
                        step={1}
                        value={scrubFrame}
                        aria-label={t("media.timeline")}
                        onChange={(event) => setScrubFrame(Number(event.target.value))}
                        onPointerUp={(event) => void selectVideoFrame(
                          selectedSource, "frame", Number(event.currentTarget.value),
                        )}
                        onKeyUp={(event) => void selectVideoFrame(
                          selectedSource, "frame", Number(event.currentTarget.value),
                        )}
                      />
                      <div className="filmstrip" aria-label={t("media.frameWindow")}>
                        {frameWindow.frames.map((frame) => {
                          const sampled = sourceSamples.some(
                            (sample) =>
                              sample.streamIndex === selectedSource.selectedStreamIndex &&
                              sample.frameIndex === frame.frame_index,
                          );
                          return (
                            <button
                              type="button"
                              key={frame.frame_index}
                              className={`${frame.frame_index === currentFrame ? "active" : ""} ${sampled ? "sampled" : ""}`}
                              aria-current={frame.frame_index === currentFrame ? "true" : undefined}
                              aria-label={`${t("media.frameNumber")} ${frame.frame_index}`}
                              onClick={() => void selectVideoFrame(selectedSource, "frame", frame.frame_index)}
                            >
                              {thumbnailUrls[frame.frame_index] ? (
                                <img
                                  className="filmstrip-thumbnail"
                                  src={thumbnailUrls[frame.frame_index]}
                                  alt=""
                                  draggable={false}
                                />
                              ) : (
                                <span className="filmstrip-thumbnail-placeholder">
                                  <LoaderCircle className="spin" size={13} />
                                </span>
                              )}
                              <span className="filmstrip-meta">
                                <strong>{frame.frame_index}</strong>
                                <span>{frame.picture_type ?? "-"}</span>
                              </span>
                            </button>
                          );
                        })}
                      </div>
                    </>
                  ) : (
                    <>
                      <div className="frame-controls">
                        <label>
                          <span>{t("media.timestamp")}</span>
                          <input
                            value={timeInput}
                            inputMode="decimal"
                            onChange={(event) => setTimeInput(event.target.value)}
                            onKeyDown={(event) => {
                              if (event.key === "Enter") showNearbyTime(selectedSource);
                            }}
                          />
                        </label>
                        <button
                          className="icon-button reveal-button"
                          type="button"
                          title={t("media.goToTime")}
                          aria-label={t("media.goToTime")}
                          onClick={() => showNearbyTime(selectedSource)}
                        >
                          <LocateFixed size={14} />
                        </button>
                        <span className="frame-index-status" role="status">
                          {indexBusy ? <LoaderCircle className="spin" size={13} /> : null}
                          {indexBusy ? t("media.indexingFrames") : t("media.indexUnavailable")}
                        </span>
                      </div>
                      <input
                        className="frame-timeline"
                        type="range"
                        min={0}
                        max={Math.max(0, selectedSource.durationSeconds ?? 0)}
                        step={0.001}
                        value={Math.min(Number(timeInput) || 0, selectedSource.durationSeconds ?? 0)}
                        disabled={!selectedSource.durationSeconds}
                        aria-label={t("media.timeline")}
                        onChange={(event) => setTimeInput(Number(event.target.value).toFixed(3))}
                        onPointerUp={(event) => void selectVideoFrame(
                          selectedSource, "timestamp", undefined,
                          Number(event.currentTarget.value),
                        )}
                        onKeyUp={(event) => void selectVideoFrame(
                          selectedSource, "timestamp", undefined,
                          Number(event.currentTarget.value),
                        )}
                      />
                    </>
                  )}
                </div>
              ) : null}

              <div className="source-metadata">
                <span>{t("media.dimensions")} <strong>{dimensionText(selectedSource?.width, selectedSource?.height)}</strong></span>
                <span>{t("media.type")} <strong>{t(`media.kind.${selectedSource.kind}`)}</strong></span>
                <span>{t("media.decoder")} <strong>{selectedSource.decoder ?? "-"}</strong></span>
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
              onClick={() => addSample(false)}
            >
              {selectedSource?.kind === "video" ? <Plus size={15} /> : <ImagePlus size={15} />}
              {selectedSource?.kind === "video" ? t("media.addCurrentFrame") : t("media.addImage")}
            </button>
            {selectedSource?.kind === "video" ? (
              <button className="secondary-button" type="button" disabled={!frameWindow || state.project.readOnly} onClick={() => addSample(true)}>
                <ChevronRight size={15} />
                {t("media.addAndContinue")}
              </button>
            ) : null}
          </div>
          <p className={`selection-note ${alreadySelected ? "visible" : ""}`} aria-live="polite">
            {alreadySelected ? t("media.alreadySelected") : " "}
          </p>
          <div className="selected-sample-list">
            {sourceSamples.map((sample) => (
              <div className="selected-sample-row" key={sample.id}>
                <div>
                  <strong>{sample.label}</strong>
                  <span>{sample.frameIndex == null ? dimensionText(selectedSource?.width, selectedSource?.height) : `${t("media.frameNumber")} ${sample.frameIndex} · ${formatSeconds(sample.timestampSeconds)}`}</span>
                </div>
                <button className="icon-button" type="button" title={t("samples.remove")} aria-label={t("samples.remove")} disabled={state.project.readOnly} onClick={() => removeSample(sample.id)}>
                  <Trash2 size={14} />
                </button>
              </div>
            ))}
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

function videoSampleLabel(source: Source, frameIndex: number | undefined, t: Translator): string {
  const sourceLabel = source.label ?? fileName(source.path);
  const streamLabel =
    source.videoStreams.length > 1
      ? ` · ${t("media.videoStream")} ${source.selectedStreamIndex ?? "-"}`
      : "";
  return `${sourceLabel}${streamLabel} #${frameIndex ?? "-"}`;
}

function sourceStreamKey(source: Source): string {
  return [source.id, source.fingerprint ?? "", source.selectedStreamIndex ?? ""].join(":");
}

function isFingerprintError(detail: string): boolean {
  return detail.includes("media_fingerprint_mismatch")
    || detail.includes("media_fingerprint_error");
}

function mediaViewState(state: ProjectState): { selectedSourceId?: string } {
  const value = state.uiStateByRoute.media;
  return value && typeof value === "object" && !Array.isArray(value)
    ? (value as { selectedSourceId?: string })
    : {};
}

function sourceSummary(source: Source, t: Translator): string {
  if (source.state === "probing") return t("media.state.probing");
  if (source.state === "missing") return t("media.state.missing");
  if (source.state === "unsupported") return t("media.state.unsupported");
  if (source.state === "error") return t("media.state.error");
  return dimensionText(source.width, source.height);
}

function previewMessage(
  source: Source,
  capabilities: MediaCapabilities | null,
  t: Translator,
): string {
  if (source.state === "probing") return t("media.state.probing");
  if (source.state === "missing") return t("media.state.missing");
  if (source.state === "unsupported") return source.errorDetail ?? t("media.state.unsupported");
  if (source.kind === "animated") return t("media.animatedNotice");
  if (source.kind === "video" && !capabilities?.video_decode_available) {
    return t("media.videoBackendRequired");
  }
  return source.errorDetail ?? t("media.previewUnavailable");
}
