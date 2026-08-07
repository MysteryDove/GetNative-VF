import { invoke } from "@tauri-apps/api/core";
import type { GeometryEnvelope } from "./types";
import type { GeometrySnapshot } from "./protocol";

/**
 * Resolve a real GeometrySnapshot through the engine `geometry` command.
 * Standard mode, full-frame active region; the engine owns all profile-specific
 * rounding semantics — the UI never reimplements them.
 */
export async function resolveGeometrySnapshot(input: {
  profileId: string;
  sourceWidth: number;
  sourceHeight: number;
  baseHeight: number;
  baseWidth?: number | null;
}): Promise<GeometrySnapshot> {
  const result = await invoke<GeometryEnvelope>("engine_geometry", {
    request: {
      profile: input.profileId,
      mode: "standard",
      sourceWidth: input.sourceWidth,
      sourceHeight: input.sourceHeight,
      activeWidth: input.sourceWidth,
      activeHeight: input.sourceHeight,
      baseHeight: input.baseHeight,
      baseWidth: input.baseWidth ?? null,
    },
  });
  const g = result.payload.geometry;
  return {
    mode: "standard",
    activeWidth: input.sourceWidth,
    activeHeight: input.sourceHeight,
    canvasWidth: g.width,
    canvasHeight: g.height,
    srcLeft: g.src_left,
    srcTop: g.src_top,
    srcWidth: g.src_width,
    srcHeight: g.src_height,
    baseWidth: input.baseWidth ?? null,
    baseHeight: input.baseHeight,
    parity: null,
  };
}
