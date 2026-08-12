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
  requestFrameWindow,
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
            onSourceChanged(source.id, detail);
          }
        }
      } finally {
        if (previewRequestId.current === requestId) setPreviewBusy(false);
      }
    },
    [onSourceChanged, setError, swapPreviewUrl, swapThumbnailUrls],
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
  }, [initializeVideoSource, videoDecodeAvailable, selectedPreviewKey, showPreview, swapPreviewUrl, swapThumbnailUrls]); // eslint-disable-line react-hooks/exhaustive-deps

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

  return {
    previewUrl,
    thumbnailUrls,
    previewBusy,
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
  };
}
