import { invoke } from "@tauri-apps/api/core";
import type { GeometryEnvelope } from "./types";
import type { GeometrySnapshot } from "./protocol";

/**
 * Resolve a real GeometrySnapshot through the engine `geometry` command.
 * The engine owns all profile-specific rounding semantics; the UI only derives
 * the aspect-ratio-preserving active width for the requested base height.
 */
export async function resolveGeometrySnapshot(input: {
  profileId: string;
  sourceWidth: number;
  sourceHeight: number;
  baseHeight: number;
  baseWidth?: number | null;
}): Promise<GeometrySnapshot> {
  const activeHeight = input.baseHeight;
  const activeWidth = (input.sourceWidth / input.sourceHeight) * activeHeight;
  const mode = input.baseWidth == null ? "standard" : "pro";
  const result = await invoke<GeometryEnvelope>("engine_geometry", {
    request: {
      profile: input.profileId,
      mode,
      sourceWidth: input.sourceWidth,
      sourceHeight: input.sourceHeight,
      activeWidth,
      activeHeight,
      baseHeight: input.baseHeight,
      baseWidth: input.baseWidth ?? null,
    },
  });
  const g = result.payload.geometry;
  return {
    mode,
    activeWidth,
    activeHeight,
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
