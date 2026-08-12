import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";
import type { DecodeBackendCapability, EngineEnvelope } from "../engine/types";
import type { SourceKind, SourceState } from "../project/types";

export type MediaCapabilities = {
  still_formats: string[];
  video_decode_available: boolean;
  ffmpeg_abi: string | null;
  index_version: number | null;
  index_format: string;
  decoder_backends: DecodeBackendCapability[];
};

export type MediaProbeResult = {
  path: string;
  file_name: string;
  kind: SourceKind;
  state: SourceState;
  fingerprint: string;
  size_bytes: number;
  width: number | null;
  height: number | null;
  duration_seconds: number | null;
  decoder: string | null;
  video_streams: Array<{
    index: number;
    codec_name: string | null;
    width: number | null;
    height: number | null;
    duration_seconds: number | null;
    frame_count: number | null;
    time_base_num: number | null;
    time_base_den: number | null;
    frame_rate_num: number | null;
    frame_rate_den: number | null;
  }>;
  selected_stream_index: number | null;
  diagnostic: { code: string; detail: string } | null;
};

export type FrameIdentity = {
  frame_index: number;
  pts: number | null;
  best_effort_timestamp: number | null;
  timestamp_seconds: number | null;
  key_frame: boolean;
  picture_type: string | null;
};

export type MediaFrameWindow = {
  selected: FrameIdentity;
  frames: FrameIdentity[];
  total_frames: number;
  previous_keyframe: FrameIdentity | null;
  next_keyframe: FrameIdentity | null;
  indexed_complete: boolean;
};

export type FrameWindowTarget =
  | "frame"
  | "timestamp"
  | "previousKeyframe"
  | "nextKeyframe";

export async function getMediaCapabilities(): Promise<MediaCapabilities> {
  await invoke("engine_worker_start");
  const envelope = await invoke<EngineEnvelope>("engine_worker_capabilities");
  const media = envelope.payload.media;
  return {
    still_formats: ["png", "jpeg", "jpg", "gif", "webp", "tiff", "tif", "bmp"],
    video_decode_available: media?.available === true,
    ffmpeg_abi: media?.ffmpeg_abi ?? null,
    index_version: media?.index_version ?? null,
    index_format: media?.index_format ?? "gnvf.lwi",
    decoder_backends: envelope.payload.decode_backends ?? [],
  };
}

export function pickMediaFiles(): Promise<string[]> {
  return invoke("media_pick_files");
}

export function probeMedia(path: string): Promise<MediaProbeResult> {
  return invoke("media_probe", { request: { path } });
}

export type MediaProgress = {
  phase: string | null;
  completed: number;
  total: number;
};

export type MediaTask<T> = {
  requestId: string;
  promise: Promise<T>;
  cancel: () => Promise<void>;
};

type WireMediaEvent = {
  type?: string;
  request_id?: string;
  job_id?: string;
  phase?: string;
  completed?: number;
  total?: number;
  code?: string;
  message?: string;
  payload?: unknown;
};

let mediaRequestSequence = 0;

function mediaRequestId(): string {
  mediaRequestSequence += 1;
  return `media-req-${Date.now()}-${mediaRequestSequence}`;
}

function submitMediaTask<T>(
  type: "media_index_begin" | "media_frame_window" | "media_preview_begin" | "media_asset_batch_begin",
  body: Record<string, unknown>,
  onProgress?: (progress: MediaProgress) => void,
): MediaTask<T> {
  const requestId = mediaRequestId();
  let engineJobId: string | null = null;
  let cancelRequested = false;
  let settled = false;
  const promise = new Promise<T>((resolve, reject) => {
    let unlisten: UnlistenFn | null = null;
    void (async () => {
      try {
        unlisten = await listen<WireMediaEvent>("engine-worker-event", (event) => {
          const wire = event.payload;
          if (wire.request_id !== requestId || settled) return;
          if (wire.type === "accepted" && typeof wire.job_id === "string") {
            engineJobId = wire.job_id;
            if (cancelRequested) {
              void invoke("engine_worker_cancel", { jobId: engineJobId }).catch(() => undefined);
            }
            return;
          }
          if (wire.type === "progress") {
            onProgress?.({
              phase: typeof wire.phase === "string" ? wire.phase : null,
              completed: typeof wire.completed === "number" ? wire.completed : 0,
              total: typeof wire.total === "number" ? wire.total : 0,
            });
            return;
          }
          if (wire.type === "warning") return;
          if (wire.type === "result") {
            settled = true;
            unlisten?.();
            resolve(wire.payload as T);
            return;
          }
          if (wire.type === "cancelled" || wire.type === "error") {
            settled = true;
            unlisten?.();
            const code = wire.type === "cancelled" ? "cancelled" : wire.code ?? "media_error";
            reject(new Error(`${code}: ${wire.message ?? "media task did not complete"}`));
          }
        });
        if (cancelRequested) {
          settled = true;
          unlisten();
          reject(new Error("cancelled: media task was cancelled before submission"));
          return;
        }
        await invoke("engine_worker_media_begin", {
          request: { type, request_id: requestId, ...body },
        });
      } catch (error) {
        if (settled) return;
        settled = true;
        unlisten?.();
        reject(error instanceof Error ? error : new Error(String(error)));
      }
    })();
  });
  return {
    requestId,
    promise,
    cancel: async () => {
      if (settled) return;
      cancelRequested = true;
      if (engineJobId) await invoke("engine_worker_cancel", { jobId: engineJobId });
    },
  };
}

export type MediaIndexResult = {
  kind: "video";
  state: "ready";
  fingerprint: string;
  size_bytes: number;
  stream_index: number;
  codec: string;
  width: number;
  height: number;
  duration_seconds: number | null;
  frame_count: number;
  time_base_num: number;
  time_base_den: number;
  decoder: string;
  index_version: number;
  rebuilt: boolean;
};

export function beginMediaIndex(
  request: { path: string; fingerprint?: string | null; streamIndex?: number | null },
  onProgress?: (progress: MediaProgress) => void,
): MediaTask<MediaIndexResult> {
  return submitMediaTask("media_index_begin", {
    path: request.path,
    fingerprint: request.fingerprint ?? null,
    stream_index: request.streamIndex ?? null,
    decoder: "auto",
  }, onProgress);
}

export function requestFrameWindow(request: {
  path: string;
  fingerprint?: string | null;
  streamIndex: number;
  target: FrameWindowTarget;
  frameIndex?: number | null;
  timestampSeconds?: number | null;
  windowRadius?: number;
}): MediaTask<MediaFrameWindow> {
  return submitMediaTask("media_frame_window", {
    path: request.path,
    fingerprint: request.fingerprint ?? null,
    stream_index: request.streamIndex,
    target: request.target.replace(/[A-Z]/g, (letter) => `_${letter.toLowerCase()}`),
    frame_index: request.frameIndex ?? null,
    timestamp_seconds: request.timestampSeconds ?? null,
    window_radius: request.windowRadius ?? 12,
  });
}

export function requestMediaPreview(request: {
  path: string;
  fingerprint?: string | null;
  streamIndex?: number | null;
  frameIndex?: number | null;
  timestampSeconds?: number | null;
  exact?: boolean;
  maxDimension?: number;
}): MediaTask<string> {
  if (request.streamIndex == null) {
    const requestId = mediaRequestId();
    const promise = invoke<ArrayBuffer | Uint8Array | number[]>("media_preview", { request })
      .then(previewBytesToUrl);
    return { requestId, promise, cancel: async () => undefined };
  }
  const task = submitMediaTask<{
    asset: { path: string };
  }>("media_preview_begin", {
    path: request.path,
    fingerprint: request.fingerprint ?? null,
    stream_index: request.streamIndex,
    target: request.timestampSeconds != null ? "timestamp" : "frame",
    frame_index: request.frameIndex ?? null,
    timestamp_seconds: request.timestampSeconds ?? null,
    maximum_dimension: request.maxDimension ?? 1600,
  });
  return {
    requestId: task.requestId,
    cancel: task.cancel,
    promise: task.promise.then(async (result) => {
      const payload = await invoke<ArrayBuffer | Uint8Array | number[]>(
        "engine_worker_media_read_asset",
        { path: result.asset.path },
      );
      return previewBytesToUrl(payload);
    }),
  };
}

function previewBytesToUrl(payload: ArrayBuffer | Uint8Array | number[]): string {
  const bytes =
    payload instanceof ArrayBuffer
      ? new Uint8Array(payload)
      : payload instanceof Uint8Array
        ? payload
        : new Uint8Array(payload);
  return URL.createObjectURL(new Blob([bytes], { type: "image/png" }));
}

export function getMediaPreview(
  request: Parameters<typeof requestMediaPreview>[0],
): Promise<string> {
  return requestMediaPreview(request).promise;
}

export type MediaFrameAsset = {
  path: string;
  format: "f32le";
  width: number;
  height: number;
  from_cache: boolean;
};

/**
 * Export one frame as an engine frame asset (f32le luma file). The returned
 * path feeds `frameAsset` in worker analyze requests; pixels never cross JSON.
 */
export function exportFrameAsset(request: {
  path: string;
  fingerprint?: string | null;
  streamIndex?: number | null;
  frameIndex?: number | null;
  width?: number | null;
  height?: number | null;
}): Promise<MediaFrameAsset> {
  if (request.streamIndex != null && request.frameIndex != null) {
    if (request.width == null || request.height == null) {
      return Promise.reject(new Error("frame_asset_invalid: video dimensions are required"));
    }
    return requestFrameAssetBatch({
      path: request.path,
      fingerprint: request.fingerprint,
      streamIndex: request.streamIndex,
      width: request.width,
      height: request.height,
      frames: [{ itemId: "frame", frameIndex: request.frameIndex }],
    }).promise.then((result) => {
      const asset = result.assets[0];
      if (!asset || asset.format !== "f32le") {
        throw new Error("frame_asset_error: engine did not return the requested frame asset");
      }
      return {
        path: asset.path,
        format: "f32le",
        width: asset.width,
        height: asset.height,
        from_cache: asset.from_cache,
      };
    });
  }
  return invoke("media_frame_asset", { request });
}

export type MediaFrameBatchPrepareRequest = {
  path: string;
  fingerprint?: string | null;
  streamIndex: number;
  width: number;
  height: number;
  frames: Array<{ itemId: string; frameIndex: number }>;
};

export type MediaFrameBatchAssetEvent = MediaFrameAsset & {
  ticket: string;
  itemId: string;
  frameIndex: number;
};

type MediaAssetBatchResult = {
  assets: Array<{
    item_id: string;
    frame_index: number;
    path: string;
    format: "f32le" | "png";
    width: number;
    height: number;
    from_cache: boolean;
  }>;
  decoded_frames: number;
};

export type MediaPreviewBatchResult = {
  assets: Array<{ itemId: string; frameIndex: number; url: string; width: number; height: number }>;
  decodedFrames: number;
};

export function requestMediaPreviewBatch(request: {
  path: string;
  fingerprint?: string | null;
  streamIndex: number;
  frames: Array<{ itemId: string; frameIndex: number; maxDimension: number }>;
}): MediaTask<MediaPreviewBatchResult> {
  const task = submitMediaTask<MediaAssetBatchResult>("media_asset_batch_begin", {
    path: request.path,
    fingerprint: request.fingerprint ?? null,
    stream_index: request.streamIndex,
    assets: request.frames.map((frame) => ({
      item_id: frame.itemId,
      frame_index: frame.frameIndex,
      format: "png",
      maximum_dimension: frame.maxDimension,
    })),
  });
  return {
    requestId: task.requestId,
    cancel: task.cancel,
    promise: task.promise.then(async (result) => ({
      decodedFrames: result.decoded_frames,
      assets: await Promise.all(result.assets.map(async (asset) => {
        const payload = await invoke<ArrayBuffer | Uint8Array | number[]>(
          "engine_worker_media_read_asset",
          { path: asset.path },
        );
        return {
          itemId: asset.item_id,
          frameIndex: asset.frame_index,
          url: previewBytesToUrl(payload),
          width: asset.width,
          height: asset.height,
        };
      })),
    })),
  };
}

export function requestFrameAssetBatch(
  request: MediaFrameBatchPrepareRequest,
): MediaTask<MediaAssetBatchResult> {
  return submitMediaTask("media_asset_batch_begin", {
    path: request.path,
    fingerprint: request.fingerprint ?? null,
    stream_index: request.streamIndex,
    width: request.width,
    height: request.height,
    assets: request.frames.map((frame) => ({
      item_id: frame.itemId,
      frame_index: frame.frameIndex,
      format: "f32le",
    })),
  });
}

export async function exportFrameAssetBatch(
  request: MediaFrameBatchPrepareRequest,
  onAsset: (asset: MediaFrameBatchAssetEvent) => void | Promise<void>,
): Promise<void> {
  for (let offset = 0; offset < request.frames.length; offset += 26) {
    const frames = request.frames.slice(offset, offset + 26);
    const result = await requestFrameAssetBatch({ ...request, frames }).promise;
    if (result.assets.length !== frames.length) {
      throw new Error(
        `frame_asset_error: media batch produced ${result.assets.length} of ${frames.length} assets`,
      );
    }
    await Promise.all(result.assets.map((asset) => onAsset({
      path: asset.path,
      format: "f32le",
      width: asset.width,
      height: asset.height,
      from_cache: asset.from_cache,
      ticket: "engine-media-batch",
      itemId: asset.item_id,
      frameIndex: asset.frame_index,
    })));
  }
}
