import type { AxisMode, BaseMode, GeometrySnapshot, GeometryWire } from "./protocol";

/** Smallest integer at least `src` with the requested parity. */
export function minimumBaseForParity(src: number, parity: "even" | "odd"): number {
  if (!Number.isFinite(src) || src <= 0) throw new Error("src dimension must be positive");
  let base = Math.ceil(src);
  if ((base & 1) !== (parity === "odd" ? 1 : 0)) base += 1;
  return base;
}

export function baseForMode(src: number, mode: BaseMode): number | null {
  return mode === "integer" ? null : minimumBaseForParity(src, mode);
}

/** Candidate value for the primary scan axis. Canvas size is never a candidate. */
export function geometryCandidate(geometry: GeometrySnapshot, axisMode: AxisMode): number {
  const src = axisMode === "w_only" ? geometry.srcWidth : geometry.srcHeight;
  const canvas = axisMode === "w_only" ? geometry.canvasWidth : geometry.canvasHeight;
  // Schema-1 recipes sometimes stored the source rectangle as the original
  // frame and the base field as the actual candidate. That shape is provably
  // legacy when src is larger than its canvas; preserve it until migrated.
  if (src > canvas) {
    const legacyBase = axisMode === "w_only" ? geometry.baseWidth : geometry.baseHeight;
    if (legacyBase != null) return legacyBase;
  }
  return src;
}

function roundToEven(value: number): number {
  const lower = Math.floor(value);
  const fraction = value - lower;
  if (fraction < 0.5) return lower;
  if (fraction > 0.5) return lower + 1;
  return lower % 2 === 0 ? lower : lower + 1;
}

function canvasFor(src: number, base: number | null | undefined): { canvas: number; offset: number; resolvedSrc: number } {
  if (base == null) {
    const canvas = roundToEven(src);
    return { canvas, offset: 0, resolvedSrc: canvas };
  }
  if (!Number.isInteger(base) || base <= 0) throw new Error("base must be a positive integer");
  const canvas = base - 2 * Math.trunc((base - src) / 2);
  if (canvas <= 0) throw new Error("resolved canvas must be positive");
  return { canvas, offset: (canvas - src) / 2, resolvedSrc: src };
}

export type GeometryValues = {
  sourceWidth: number;
  sourceHeight: number;
  srcWidth: number;
  srcHeight: number;
  baseWidth?: number | null;
  baseHeight?: number | null;
  mode?: "standard" | "pro";
};

/** Interpret one scan selection according to the active scan axis. */
export function srcFromScanSelection(input: {
  axisMode: AxisMode;
  selected: number;
  sourceWidth: number;
  sourceHeight: number;
  independentSrcWidth?: number | null;
  independentSrcHeight?: number | null;
}): { srcWidth: number; srcHeight: number } {
  if (!Number.isFinite(input.selected) || input.selected <= 0) {
    throw new Error("scan selection must be positive");
  }
  if (input.axisMode === "h_only") {
    return { srcWidth: input.sourceWidth, srcHeight: input.selected };
  }
  if (input.axisMode === "w_only") {
    return { srcWidth: input.selected, srcHeight: input.sourceHeight };
  }
  return {
    srcWidth: input.independentSrcWidth ?? input.sourceWidth * input.selected / input.sourceHeight,
    srcHeight: input.independentSrcHeight ?? input.selected,
  };
}

/** Resolve the same independent-axis parity geometry as the engine. */
export function resolveGeometryValues(input: GeometryValues): GeometrySnapshot {
  const values = [
    input.sourceWidth,
    input.sourceHeight,
    input.srcWidth,
    input.srcHeight,
  ];
  if (values.some((value) => !Number.isFinite(value) || value <= 0)) {
    throw new Error("source and src dimensions must be positive finite numbers");
  }
  const width = canvasFor(input.srcWidth, input.baseWidth);
  const height = canvasFor(input.srcHeight, input.baseHeight);
  return {
    mode: input.mode ?? (input.baseWidth == null && input.baseHeight == null ? "standard" : "pro"),
    sourceWidth: input.sourceWidth,
    sourceHeight: input.sourceHeight,
    activeWidth: width.resolvedSrc,
    activeHeight: height.resolvedSrc,
    canvasWidth: width.canvas,
    canvasHeight: height.canvas,
    srcLeft: width.offset,
    srcTop: height.offset,
    srcWidth: input.baseWidth == null ? width.resolvedSrc : input.srcWidth,
    srcHeight: input.baseHeight == null ? height.resolvedSrc : input.srcHeight,
    baseWidth: input.baseWidth ?? null,
    baseHeight: input.baseHeight ?? null,
    parity: null,
  };
}

/** Re-resolve a locked geometry for another source shape without reusing absolute canvas pixels. */
export function geometryForSource(
  geometry: GeometrySnapshot,
  axisMode: AxisMode,
  sourceWidth: number,
  sourceHeight: number,
): GeometrySnapshot {
  let srcWidth = geometry.srcWidth;
  let srcHeight = geometry.srcHeight;
  if (axisMode === "h_only") {
    srcWidth = sourceWidth;
  } else if (axisMode === "w_only") {
    srcHeight = sourceHeight;
  } else {
    // H+W keeps the selected height and follows each source's aspect ratio.
    srcWidth = sourceWidth * srcHeight / sourceHeight;
  }
  return resolveGeometryValues({
    sourceWidth,
    sourceHeight,
    srcWidth,
    srcHeight,
    baseWidth: geometry.baseWidth ?? null,
    baseHeight: geometry.baseHeight ?? null,
    mode: geometry.mode,
  });
}

export function geometryToWire(geometry: GeometrySnapshot): GeometryWire {
  return {
    width: geometry.canvasWidth,
    height: geometry.canvasHeight,
    srcLeft: geometry.srcLeft,
    srcTop: geometry.srcTop,
    srcWidth: geometry.srcWidth,
    srcHeight: geometry.srcHeight,
    baseWidth: geometry.baseWidth ?? null,
    baseHeight: geometry.baseHeight ?? null,
  };
}

/** Normalize old manifest geometry without guessing ambiguous fractional fields. */
export function migrateGeometrySnapshot(value: unknown): GeometrySnapshot | null {
  if (!value || typeof value !== "object") return null;
  const raw = value as Record<string, unknown>;
  const snake = (key: string) => key.replace(/[A-Z]/g, (letter) => `_${letter.toLowerCase()}`);
  const rawValue = (key: string) => raw[key] ?? raw[snake(key)];
  const number = (key: string): number | null => {
    const value = rawValue(key);
    return typeof value === "number" && Number.isFinite(value) ? value : null;
  };
  const storedCanvasWidth = number("canvasWidth");
  const storedCanvasHeight = number("canvasHeight");
  const storedActiveWidth = number("activeWidth");
  const storedActiveHeight = number("activeHeight");
  const canvasWidth = storedCanvasWidth ?? (storedActiveWidth != null ? roundToEven(storedActiveWidth) : null);
  const canvasHeight = storedCanvasHeight ?? (storedActiveHeight != null ? roundToEven(storedActiveHeight) : null);
  if (canvasWidth == null || canvasHeight == null || canvasWidth <= 0 || canvasHeight <= 0) return null;
  const srcWidth = number("srcWidth");
  const srcHeight = number("srcHeight");
  const hasExplicitSrcFields = srcWidth != null && srcHeight != null;
  const hasExplicitFractionalFields =
    hasExplicitSrcFields && number("srcLeft") != null && number("srcTop") != null;
  const migrated = {
    mode: raw.mode === "pro" ? "pro" : "standard",
    sourceWidth: number("sourceWidth") ?? undefined,
    sourceHeight: number("sourceHeight") ?? undefined,
    activeWidth: srcWidth ?? storedActiveWidth ?? canvasWidth,
    activeHeight: srcHeight ?? storedActiveHeight ?? canvasHeight,
    canvasWidth,
    canvasHeight,
    srcLeft: number("srcLeft") ?? 0,
    srcTop: number("srcTop") ?? 0,
    srcWidth: srcWidth ?? storedActiveWidth ?? canvasWidth,
    srcHeight: srcHeight ?? storedActiveHeight ?? canvasHeight,
    baseWidth: hasExplicitSrcFields && (rawValue("baseWidth") === null || typeof rawValue("baseWidth") === "number")
      ? rawValue("baseWidth") as number | null | undefined ?? null
      : null,
    baseHeight: hasExplicitSrcFields && (rawValue("baseHeight") === null || typeof rawValue("baseHeight") === "number")
      ? rawValue("baseHeight") as number | null | undefined ?? null
      : null,
    parity: raw.parity === "even" || raw.parity === "odd" ? raw.parity : null,
    needsReview: raw.needsReview === true ||
      (hasExplicitSrcFields && !hasExplicitFractionalFields) ||
      (storedCanvasWidth == null || storedCanvasHeight == null) ||
      (!hasExplicitFractionalFields && (storedActiveWidth != null || storedActiveHeight != null) &&
        (storedActiveWidth !== canvasWidth || storedActiveHeight !== canvasHeight)) ||
      (srcWidth != null && srcWidth !== canvasWidth && rawValue("baseWidth") === undefined) ||
      (srcHeight != null && srcHeight !== canvasHeight && rawValue("baseHeight") === undefined),
  } satisfies GeometrySnapshot;
  return migrated;
}
