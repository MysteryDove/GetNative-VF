import { invoke } from "@tauri-apps/api/core";
import type { GeometryEnvelope } from "./types";
import type { AxisMode, GeometrySnapshot } from "./protocol";
import { resolveGeometryValues } from "./geometry";

export type GeometryResolveInput = {
  profileId: string;
  sourceWidth: number;
  sourceHeight: number;
  axisMode?: AxisMode;
  /** Selected native dimensions. `srcHeight` remains the primary legacy input. */
  srcWidth?: number;
  srcHeight?: number;
  /** Legacy alias for srcHeight. */
  candidate?: number;
  baseHeight?: number | null;
  baseWidth?: number | null;
};

function selectedSourceGeometry(input: GeometryResolveInput): { srcWidth: number; srcHeight: number } {
  const axis = input.axisMode ?? "h_plus_w";
  const srcHeight = input.srcHeight ?? input.candidate ?? input.sourceHeight;
  const srcWidth =
    input.srcWidth ??
    (axis === "w_only"
      ? input.candidate ?? input.sourceWidth
      : input.sourceWidth * srcHeight / input.sourceHeight);
  return { srcWidth, srcHeight };
}

/** Resolve through the engine, preserving the measured src/base/offset values. */
export async function resolveGeometrySnapshot(input: GeometryResolveInput): Promise<GeometrySnapshot> {
  const selected = selectedSourceGeometry(input);
  const mode = input.baseHeight == null && input.baseWidth == null ? "standard" : "pro";
  const result = await invoke<GeometryEnvelope>("engine_geometry", {
    request: {
      profile: input.profileId,
      mode,
      sourceWidth: input.sourceWidth,
      sourceHeight: input.sourceHeight,
      activeWidth: selected.srcWidth,
      activeHeight: selected.srcHeight,
      baseHeight: input.baseHeight ?? null,
      baseWidth: input.baseWidth ?? null,
    },
  });
  const g = result.payload.geometry;
  return {
    mode,
    sourceWidth: input.sourceWidth,
    sourceHeight: input.sourceHeight,
    activeWidth: g.src_width,
    activeHeight: g.src_height,
    canvasWidth: g.width,
    canvasHeight: g.height,
    srcLeft: g.src_left,
    srcTop: g.src_top,
    srcWidth: g.src_width,
    srcHeight: g.src_height,
    baseWidth: input.baseWidth ?? null,
    baseHeight: input.baseHeight ?? null,
    parity: null,
  };
}

/** Synchronous equivalent used to resolve one locked recipe per source shape. */
export function resolveGeometrySnapshotLocal(input: GeometryResolveInput): GeometrySnapshot {
  const selected = selectedSourceGeometry(input);
  return resolveGeometryValues({
    sourceWidth: input.sourceWidth,
    sourceHeight: input.sourceHeight,
    srcWidth: selected.srcWidth,
    srcHeight: selected.srcHeight,
    baseHeight: input.baseHeight ?? null,
    baseWidth: input.baseWidth ?? null,
    mode: input.baseHeight == null && input.baseWidth == null ? "standard" : "pro",
  });
}
