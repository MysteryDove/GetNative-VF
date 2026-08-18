/**
 * Preview lifecycle for the Media page: owns the object URLs (preview +
 * filmstrip thumbnails), request cancellation tokens, the frame-window
 * session, and the viewport display state that resets with the source.
 *
 * Object-URL ownership rule: `swapPreviewUrl`/`swapThumbnailUrls` revoke the
 * URLs they replace; in-flight requests revoke their own results when stale;
 * the unmount effect revokes whatever is still live.
 */

import { useCallback, useEffect, useRef, useState } from "react";
import {
  requestMediaPreview,
  requestMediaPreviewBatch,
  type FrameWindowTarget,
  type MediaFrameWindow,
} from "../media/service";
import { isFingerprintError, sourceStreamKey } from "../media/helpers";
import type { Source } from "../project/types";

export type SelectVideoFrame = (
  source: Source,
  target: FrameWindowTarget,
  frameIndex?: number,
  timestampSeconds?: number,
) => Promise<void>;

/** Draft-preview cadence while the progress slider is being dragged. */
const SCRUB_INTERVAL_MS = 120;

function revokeObjectUrl(url: string | null): void {
  if (url?.startsWith("blob:")) URL.revokeObjectURL(url);
}

/**
 * Decoded-frame progress per source id, kept only while the source stays in
 * the probing state — entries clear once indexing completes (or the source
 * is removed), so the source list never shows stale counts.
 */
export function useIndexProgress(
  sourcesById: Record<string, Source>,
): [Record<string, number>, (sourceId: string, decodedFrames: number) => void] {
  const [indexProgress, setIndexProgress] = useState<Record<string, number>>({});
  useEffect(() => {
    setIndexProgress((current) => {
      const next = Object.fromEntries(
        Object.entries(current).filter(([id]) => sourcesById[id]?.state === "probing"),
      );
      return Object.keys(next).length === Object.keys(current).length ? current : next;
    });
  }, [sourcesById]);
  const reportProgress = useCallback((sourceId: string, decodedFrames: number) => {
    setIndexProgress((current) => ({ ...current, [sourceId]: decodedFrames }));
  }, []);
  return [indexProgress, reportProgress];
}

export function useMediaPreview({
  selectedSource,
  videoDecodeAvailable,
  setError,
  onSourceChanged,
  onSourceMissing,
}: {
  selectedSource: Source | null;
  videoDecodeAvailable: boolean | undefined;
  setError: (detail: string) => void;
  onSourceChanged: (sourceId: string, detail: string) => void;
  onSourceMissing: (sourceId: string, detail: string) => void;
}) {
  const [frameWindowState, setFrameWindowState] = useState<{
    sourceKey: string;
    window: MediaFrameWindow;
  } | null>(null);
  const [previewUrl, setPreviewUrl] = useState<string | null>(null);
  const [thumbnailUrls, setThumbnailUrls] = useState<Record<number, string>>({});
  const [previewBusy, setPreviewBusy] = useState(false);
  const [previewDecoder, setPreviewDecoder] = useState<string | null>(null);
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
  /** Throttled drag state: latest slider frame awaiting a draft decode. */
  const scrubTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const scrubPending = useRef<{ source: Source; frameIndex: number } | null>(null);
  const scrubLastFire = useRef(0);

  const cancelPendingScrub = useCallback(() => {
    scrubPending.current = null;
    if (scrubTimer.current !== null) {
      clearTimeout(scrubTimer.current);
      scrubTimer.current = null;
    }
  }, []);

  /** Atomic preview swap: the old image stays visible until its replacement exists. */
  const swapPreviewUrl = useCallback((next: string | null) => {
    const previous = previewUrlRef.current;
    previewUrlRef.current = next;
    setPreviewUrl(next);
    if (previous !== next) revokeObjectUrl(previous);
  }, []);

  const swapThumbnailUrls = useCallback((next: Record<number, string>) => {
    for (const url of Object.values(thumbnailUrlsRef.current)) revokeObjectUrl(url);
    thumbnailUrlsRef.current = next;
    setThumbnailUrls(next);
  }, []);

  const frameWindow =
    selectedSource && frameWindowState?.sourceKey === sourceStreamKey(selectedSource)
      ? frameWindowState.window
      : null;

  const showPreview = useCallback(async (
    request: Parameters<typeof requestMediaPreview>[0],
    sourceId?: string,
  ) => {
    const requestId = ++previewRequestId.current;
    await activeMediaTask.current?.cancel().catch(() => undefined);
    if (previewRequestId.current !== requestId) return;
    const task = requestMediaPreview(request);
    activeMediaTask.current = task;
    setPreviewBusy(true);
    try {
      const { url: nextUrl, decoder } = await task.promise;
      if (previewRequestId.current !== requestId) {
        revokeObjectUrl(nextUrl);
        return;
      }
      swapPreviewUrl(nextUrl);
      setPreviewDecoder(decoder);
      setError("");
    } catch (reason) {
      // Keep the last good preview on screen; the error banner carries the detail.
      if (previewRequestId.current === requestId) {
        const detail = String(reason);
        setError(detail);
        previewKeyRef.current = "";
        if (sourceId && isFingerprintError(detail)) {
          onSourceChanged(sourceId, detail);
        } else if (sourceId && detail.includes("media_missing")) {
          onSourceMissing(sourceId, detail);
        }
      }
    } finally {
      if (previewRequestId.current === requestId) setPreviewBusy(false);
    }
  }, [onSourceChanged, onSourceMissing, setError, swapPreviewUrl]);

  const selectVideoFrame: SelectVideoFrame = useCallback(
    async (source, target, frameIndex, timestampSeconds) => {
      const streamIndex = source.selectedStreamIndex ?? source.videoStreams[0]?.index;
      if (streamIndex === undefined) return;
      cancelPendingScrub();
      const requestId = ++previewRequestId.current;
      await activeMediaTask.current?.cancel().catch(() => undefined);
      if (previewRequestId.current !== requestId) return;
      setPreviewBusy(true);
      try {
        // One round trip per seek: the rail's frame window rides along with
        // the preview PNG in the worker reply.
        const mainTask = requestMediaPreview({
          path: source.path,
          fingerprint: source.fingerprint,
          streamIndex,
          target,
          frameIndex,
          timestampSeconds,
          maxDimension: 1600,
          windowRadius: 12,
        });
        activeMediaTask.current = mainTask;
        const { url, window, decoder } = await mainTask.promise;
        if (previewRequestId.current !== requestId) return;
        if (!window) {
          throw new Error("media_decode_error: preview reply omitted the frame window");
        }
        setFrameWindowState({ sourceKey: sourceStreamKey(source), window });
        setFrameInput(String(window.selected.frame_index));
        setScrubFrame(window.selected.frame_index);
        setTimeInput((window.selected.timestamp_seconds ?? 0).toFixed(3));
        swapPreviewUrl(url);
        setPreviewDecoder(decoder);
        setError("");

        // The main image is the interactive seek result. Filmstrip thumbnails
        // continue in the background so they cannot delay the first visible
        // frame; a later seek cancels this task through activeMediaTask.
        setPreviewBusy(false);
        const thumbnailTask = requestMediaPreviewBatch({
          path: source.path,
          fingerprint: source.fingerprint,
          streamIndex,
          frames: window.frames.slice(0, 25).map((frame) => ({
            itemId: `thumb-${frame.frame_index}`,
            frameIndex: frame.frame_index,
            maxDimension: 160,
          })),
        });
        activeMediaTask.current = thumbnailTask;
        void thumbnailTask.promise.then((batch) => {
          if (previewRequestId.current !== requestId) return;
          swapThumbnailUrls(Object.fromEntries(
            batch.assets.map((asset) => [asset.frameIndex, asset.url]),
          ));
        }).catch((reason) => {
          if (previewRequestId.current !== requestId) return;
          const detail = String(reason);
          if (!detail.includes("cancelled:")) setError(detail);
          if (isFingerprintError(detail)) onSourceChanged(source.id, detail);
        });
      } catch (reason) {
        if (previewRequestId.current === requestId) {
          const detail = String(reason);
          setError(detail);
          if (isFingerprintError(detail)) {
            onSourceChanged(source.id, detail);
          }
        }
      } finally {
        if (previewRequestId.current === requestId) setPreviewBusy(false);
      }
    },
    [cancelPendingScrub, onSourceChanged, setError, swapPreviewUrl, swapThumbnailUrls],
  );

  const fireScrub = useCallback(() => {
    const pending = scrubPending.current;
    if (!pending) return;
    scrubPending.current = null;
    scrubLastFire.current = Date.now();
    const { source, frameIndex } = pending;
    const streamIndex = source.selectedStreamIndex ?? source.videoStreams[0]?.index;
    if (streamIndex === undefined) return;
    const requestId = ++previewRequestId.current;
    void activeMediaTask.current?.cancel().catch(() => undefined);
    // Draft frames decode at the release resolution, so the follow-up request
    // fired on pointer-up hits the worker's on-disk PNG cache and only has to
    // serve the file.
    const task = requestMediaPreview({
      path: source.path,
      fingerprint: source.fingerprint,
      streamIndex,
      frameIndex,
      maxDimension: 1600,
      windowRadius: 0,
    });
    activeMediaTask.current = task;
    void task.promise.then(({ url, decoder }) => {
      if (previewRequestId.current !== requestId) return;
      swapPreviewUrl(url);
      setPreviewDecoder(decoder);
    }).catch(() => undefined);
  }, [swapPreviewUrl]);

  const scrubVideoFrame = useCallback((source: Source, frameIndex: number) => {
    setScrubFrame(frameIndex);
    scrubPending.current = { source, frameIndex };
    if (scrubTimer.current !== null) return;
    const elapsed = Date.now() - scrubLastFire.current;
    if (elapsed >= SCRUB_INTERVAL_MS) {
      fireScrub();
    } else {
      scrubTimer.current = setTimeout(() => {
        scrubTimer.current = null;
        fireScrub();
      }, SCRUB_INTERVAL_MS - elapsed);
    }
  }, [fireScrub]);

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
          onSourceChanged(source.id, detail);
        }
      } finally {
        if (sourceSessionId.current === sessionId) setIndexBusy(false);
      }
    },
    [onSourceChanged, selectVideoFrame, setError],
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
    cancelPendingScrub();
    setFrameWindowState(null);
    setPreviewDecoder(null);
    setIndexBusy(false);
    setZoom(1);
    setPixelPosition(null);
    previewRequestId.current += 1;
    void activeMediaTask.current?.cancel().catch(() => undefined);
    swapPreviewUrl(null);
    swapThumbnailUrls({});
    if (!selectedSource || !selectedPreviewKey) return;
    if (selectedSource.kind === "video") {
      if (videoDecodeAvailable) {
        void initializeVideoSource(selectedSource, sessionId);
      }
    } else {
      void showPreview(
        { path: selectedSource.path, fingerprint: selectedSource.fingerprint },
        selectedSource.id,
      );
    }
    // selectedSource is read from the render that produced selectedPreviewKey.
  }, [initializeVideoSource, videoDecodeAvailable, selectedPreviewKey, showPreview, swapPreviewUrl, swapThumbnailUrls, cancelPendingScrub]); // eslint-disable-line react-hooks/exhaustive-deps

  // Revoke the live object URL only on unmount; in-flight swaps own their URLs.
  useEffect(
    () => () => {
      previewRequestId.current += 1;
      cancelPendingScrub();
      void activeMediaTask.current?.cancel().catch(() => undefined);
      revokeObjectUrl(previewUrlRef.current);
      for (const url of Object.values(thumbnailUrlsRef.current)) revokeObjectUrl(url);
    },
    [cancelPendingScrub],
  );

  return {
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
    setScrubFrame,
    zoom,
    setZoom,
    pixelPosition,
    setPixelPosition,
    selectVideoFrame,
    scrubVideoFrame,
  };
}
