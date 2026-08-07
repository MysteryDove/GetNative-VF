import { invoke } from "@tauri-apps/api/core";
import type { SourceKind, SourceState } from "../project/types";

export type MediaToolCapability = {
  available: boolean;
  source: string;
  path: string | null;
  version: string | null;
};

export type MediaCapabilities = {
  still_formats: string[];
  ffmpeg: MediaToolCapability;
  ffprobe: MediaToolCapability;
  video_decode_available: boolean;
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

export function getMediaCapabilities(): Promise<MediaCapabilities> {
  return invoke("media_capabilities");
}

export function pickMediaFiles(): Promise<string[]> {
  return invoke("media_pick_files");
}

export function probeMedia(path: string): Promise<MediaProbeResult> {
  return invoke("media_probe", { request: { path } });
}

export function getFrameWindow(request: {
  path: string;
  fingerprint?: string | null;
  streamIndex: number;
  target: FrameWindowTarget;
  frameIndex?: number | null;
  timestampSeconds?: number | null;
  windowRadius?: number;
}): Promise<MediaFrameWindow> {
  return invoke("media_frame_window", { request });
}

export async function getMediaPreview(request: {
  path: string;
  fingerprint?: string | null;
  streamIndex?: number | null;
  frameIndex?: number | null;
  timestampSeconds?: number | null;
  exact?: boolean;
  maxDimension?: number;
}): Promise<string> {
  const payload = await invoke<ArrayBuffer | Uint8Array | number[]>("media_preview", { request });
  const bytes =
    payload instanceof ArrayBuffer
      ? new Uint8Array(payload)
      : payload instanceof Uint8Array
        ? payload
        : new Uint8Array(payload);
  return URL.createObjectURL(new Blob([bytes], { type: "image/png" }));
}
