import { useId, useMemo, useState } from "react";
import { useElementSize } from "../hooks/useElementSize";

export type ErrorPlotDatum = {
  key: string;
  runId: string;
  /** X position as a numeric string (height candidate, frame index, …). */
  x: string;
  metric: number;
  color: string;
  /** Optional emphasis for derived/aggregate series. */
  lineWidth?: number;
  /** Series label shown in the marker tooltip, e.g. "8bit.png · Lanczos 3". */
  label?: string;
};

/*
 * Theme-aware chrome (grid, axis, threshold, selection) is class-driven via
 * CSS custom properties — see .plot-grid-line / .plot-axis-line /
 * .plot-threshold-line / .plot-selected-line and the .is-selected / .is-best
 * marker states in App.css. Only the categorical series palette stays in JS:
 * those hues read on both the dark and the light plot surface.
 */

/** Fallback series color when a datum carries no explicit color. */
export const DEFAULT_SERIES_COLOR = "#3b82f6";

/** Shared plot series palette (analyze plot, verify plot, sample swatches). */
const SERIES_PALETTE = [DEFAULT_SERIES_COLOR, "#22c55e", "#f59e0b", "#a855f7", "#ef4444", "#06b6d4"];

export function plotSeriesColor(index: number): string {
  return SERIES_PALETTE[index % SERIES_PALETTE.length];
}

const SUPERSCRIPTS: Record<string, string> = {
  "-": "⁻",
  "0": "⁰",
  "1": "¹",
  "2": "²",
  "3": "³",
  "4": "⁴",
  "5": "⁵",
  "6": "⁶",
  "7": "⁷",
  "8": "⁸",
  "9": "⁹",
};

function powerLabel(exponent: number): string {
  const sup = String(exponent)
    .split("")
    .map((char) => SUPERSCRIPTS[char] ?? char)
    .join("");
  return `10${sup}`;
}

/** Round `rawStep` to a 1/2/2.5/5 × 10^n nice step. */
function niceStep(rawStep: number): number {
  const magnitude = 10 ** Math.floor(Math.log10(rawStep));
  const fraction = rawStep / magnitude;
  if (fraction <= 1) return magnitude;
  if (fraction <= 2) return 2 * magnitude;
  if (fraction <= 2.5) return 2.5 * magnitude;
  if (fraction <= 5) return 5 * magnitude;
  return 10 * magnitude;
}

function formatTick(value: number, step: number): string {
  const decimals = step >= 10 ? 0 : step >= 1 ? 1 : 2;
  return value.toFixed(decimals);
}

/**
 * Min-max bucket decimation: for dense series, keep per bucket the lowest and
 * highest points (in x order). Preserves the deep valleys that carry the
 * descale answer, unlike naive every-Nth sampling. Applied to the visible
 * window only (see windowSlice), so zooming in re-decimates at full detail.
 */
export function decimateMinMax<T>(points: T[], value: (point: T) => number, maxBuckets: number): T[] {
  if (points.length <= maxBuckets * 2 || maxBuckets < 1) return points;
  const bucketSize = points.length / maxBuckets;
  const out: T[] = [];
  for (let bucket = 0; bucket < maxBuckets; bucket += 1) {
    const start = Math.floor(bucket * bucketSize);
    const end = Math.max(start + 1, Math.min(points.length, Math.floor((bucket + 1) * bucketSize)));
    let low = start;
    let high = start;
    for (let i = start + 1; i < end; i += 1) {
      if (value(points[i]) < value(points[low])) low = i;
      if (value(points[i]) > value(points[high])) high = i;
    }
    if (low === high) out.push(points[low]);
    else if (low < high) out.push(points[low], points[high]);
    else out.push(points[high], points[low]);
  }
  return out;
}

/** Marker budget per series; beyond it markers would drown the SVG DOM. */
export const MAX_MARKERS = 300;

/**
 * Slice an x-sorted series to the visible window, keeping the first point
 * just outside each edge so polyline segments still span the window boundary
 * instead of stopping at the first in-range sample. Decimation and the
 * marker budget both operate on this slice, which is what keeps zoomed views
 * at full resolution: unzoomed, the slice is the whole series.
 */
/** Loop min/max — `Math.min(...n)` throws RangeError once n exceeds the apply limit. */
export function extent(values: ArrayLike<number>): { min: number; max: number } | null {
  if (values.length === 0) return null;
  let min = values[0];
  let max = values[0];
  for (let i = 1; i < values.length; i += 1) {
    const value = values[i];
    if (value < min) min = value;
    if (value > max) max = value;
  }
  return { min, max };
}

export function windowSlice<T>(points: T[], x: (point: T) => number, xMin: number, xMax: number): T[] {
  let lo = 0;
  while (lo < points.length && x(points[lo]) < xMin) lo += 1;
  let hi = points.length;
  while (hi > lo && x(points[hi - 1]) > xMax) hi -= 1;
  return points.slice(Math.max(0, lo - 1), Math.min(points.length, hi + 1));
}

/**
 * X values arrive as strings from heterogeneous sources (grid draft, engine
 * rows, verdict formatting), so compare numerically with a small relative
 * tolerance instead of by exact string equality.
 */
function samePlotX(a: string | null | undefined, b: string): boolean {
  if (a == null) return false;
  const na = Number(a);
  const nb = Number(b);
  if (!Number.isFinite(na) || !Number.isFinite(nb)) return a === b;
  return Math.abs(na - nb) <= 1e-6 * Math.max(1, Math.abs(na), Math.abs(nb));
}

/**
 * getnative-style relative-error plot: one line+marker series per run,
 * numeric X, Relative error on (log) Y, dashed 1e-6 perfect-descale guide.
 * Drag across the plot to zoom into an X range (reported via
 * onZoomRangeChange); double-click or the reset button zooms back out.
 * Polyline decimation and the marker budget are scoped to the visible
 * window, so a zoomed view shows every sample instead of a globally
 * thinned subset.
 */
export function ErrorLinePlot({
  data,
  logScale,
  threshold,
  title,
  xAxisLabel,
  yAxisLabel,
  thresholdLabel,
  bestKey,
  selectedX,
  valleyKeys,
  onSelect,
  resetLabel,
  onZoomRangeChange,
}: {
  data: ErrorPlotDatum[];
  logScale: boolean;
  threshold: number;
  title: string;
  xAxisLabel: string;
  yAxisLabel: string;
  thresholdLabel: string;
  bestKey?: string | null;
  selectedX?: string | null;
  /** Point keys of detected local valleys, drawn as hollow ring markers. */
  valleyKeys?: ReadonlySet<string> | null;
  onSelect?: (x: string) => void;
  resetLabel?: string;
  onZoomRangeChange?: (range: { xMin: number; xMax: number } | null) => void;
}) {
  const [hostRef, size] = useElementSize();
  const [zoomDomain, setZoomDomain] = useState<{ xMin: number; xMax: number } | null>(null);
  const [dragRange, setDragRange] = useState<{ start: number; current: number } | null>(null);

  const series = useMemo(() => {
    const byRun = new Map<string, ErrorPlotDatum[]>();
    for (const point of data) {
      if (!Number.isFinite(Number(point.x))) continue;
      const bucket = byRun.get(point.runId) ?? [];
      bucket.push(point);
      byRun.set(point.runId, bucket);
    }
    return [...byRun.entries()].map(([runId, points]) => ({
      runId,
      points: [...points].sort((a, b) => Number(a.x) - Number(b.x)),
    }));
  }, [data]);

  const flat = useMemo(() => series.flatMap((entry) => entry.points), [series]);

  const domain = useMemo(() => {
    if (!flat.length) return null;
    let xMin = Infinity;
    let xMax = -Infinity;
    for (const point of flat) {
      const x = Number(point.x);
      if (x < xMin) xMin = x;
      if (x > xMax) xMax = x;
    }
    if (xMax - xMin < 1e-9) {
      xMin -= 0.5;
      xMax += 0.5;
    } else {
      const pad = (xMax - xMin) * 0.02;
      xMin -= pad;
      xMax += pad;
    }
    // A zoomed X window re-derives the Y domain from in-range points only.
    const visible =
      zoomDomain != null
        ? flat.filter((point) => Number(point.x) >= zoomDomain.xMin && Number(point.x) <= zoomDomain.xMax)
        : flat;
    const yPoints = visible.length ? visible : flat;
    if (zoomDomain) {
      xMin = zoomDomain.xMin;
      xMax = zoomDomain.xMax > zoomDomain.xMin ? zoomDomain.xMax : zoomDomain.xMin + 1e-9;
    }
    if (logScale) {
      let logMin = Math.log10(threshold);
      let logMax = logMin;
      for (const point of yPoints) {
        const log = Math.log10(point.metric + 1e-9);
        if (log < logMin) logMin = log;
        if (log > logMax) logMax = log;
      }
      const yMin = Math.floor(logMin);
      const yMax = Math.ceil(logMax);
      return { xMin, xMax, yMin, yMax: yMax <= yMin ? yMin + 1 : yMax };
    }
    let metricMax = threshold;
    for (const point of yPoints) {
      if (point.metric > metricMax) metricMax = point.metric;
    }
    return { xMin, xMax, yMin: 0, yMax: metricMax * 1.05 };
  }, [flat, logScale, threshold, zoomDomain]);

  function resetZoom() {
    setZoomDomain(null);
    setDragRange(null);
    onZoomRangeChange?.(null);
  }

  const width = Math.max(0, size.width);
  const height = Math.max(0, size.height);
  const margin = { top: 30, right: 18, bottom: 46, left: 62 };
  const innerWidth = Math.max(0, width - margin.left - margin.right);
  const innerHeight = Math.max(0, height - margin.top - margin.bottom);

  const clipId = useId().replace(/:/g, "");
  const yValue = (metric: number) =>
    logScale ? Math.log10(metric + 1e-9) : metric;

  /*
   * Per-series geometry, memoized so drag-to-zoom pointermove re-renders and
   * marker selection changes don't rebuild paths. Both the polyline
   * decimation and the marker stride are scoped to the visible X window:
   * unzoomed this matches the old whole-series behavior, zoomed it reveals
   * full detail instead of reusing globally thinned points.
   */
  const seriesGeometry = useMemo(() => {
    if (!domain || innerWidth <= 0 || innerHeight <= 0) return [];
    const xS = (value: number) =>
      margin.left + ((value - domain.xMin) / (domain.xMax - domain.xMin)) * innerWidth;
    const yS = (value: number) =>
      margin.top + (1 - (value - domain.yMin) / (domain.yMax - domain.yMin)) * innerHeight;
    return series.map(({ runId, points }) => {
      const visible = windowSlice(points, (point) => Number(point.x), domain.xMin, domain.xMax);
      const pathPoints = decimateMinMax(
        visible,
        (point) => yValue(point.metric),
        Math.max(64, Math.floor(innerWidth)),
      );
      const path = pathPoints
        .map(
          (point, index) =>
            `${index === 0 ? "M" : "L"}${xS(Number(point.x)).toFixed(2)},${yS(yValue(point.metric)).toFixed(2)}`,
        )
        .join(" ");
      const markerStride = Math.max(1, Math.ceil(visible.length / MAX_MARKERS));
      const markers = visible.map((point) => ({
        point,
        cx: xS(Number(point.x)),
        cy: yS(yValue(point.metric)),
      }));
      return {
        runId,
        color: points[0]?.color ?? DEFAULT_SERIES_COLOR,
        lineWidth: points[0]?.lineWidth ?? 1.6,
        path,
        markerStride,
        markers,
      };
    });
  }, [series, domain, innerWidth, innerHeight, logScale]);

  if (!domain) return <div ref={hostRef} className="error-plot" />;

  const xScale = (value: number) =>
    margin.left + ((value - domain.xMin) / (domain.xMax - domain.xMin)) * innerWidth;
  const yScale = (value: number) =>
    margin.top + (1 - (value - domain.yMin) / (domain.yMax - domain.yMin)) * innerHeight;

  const xStep = niceStep((domain.xMax - domain.xMin) / 8);
  const xTicks: number[] = [];
  for (let tick = Math.ceil(domain.xMin / xStep) * xStep; tick <= domain.xMax; tick += xStep) {
    xTicks.push(Number(tick.toFixed(6)));
  }

  const yTicks: number[] = [];
  if (logScale) {
    for (let exponent = domain.yMin; exponent <= domain.yMax; exponent += 1) {
      yTicks.push(exponent);
    }
  } else {
    const step = niceStep((domain.yMax - domain.yMin) / 4);
    for (let tick = 0; tick <= domain.yMax; tick += step) yTicks.push(tick);
  }

  const thresholdY = yScale(yValue(threshold));
  const showThreshold = thresholdY >= margin.top && thresholdY <= margin.top + innerHeight;
  const selectedPixelX =
    selectedX != null && Number.isFinite(Number(selectedX))
      ? xScale(Number(selectedX))
      : null;

  const toDataX = (event: { clientX: number; currentTarget: Element }) => {
    // currentTarget is the zoom-surface rect itself: its bounds already start
    // at the plot area's left edge, so no margin offset is subtracted here.
    const rect = event.currentTarget.getBoundingClientRect();
    const ratio = (event.clientX - rect.left) / Math.max(1, rect.width);
    const value = domain.xMin + ratio * (domain.xMax - domain.xMin);
    return Math.min(domain.xMax, Math.max(domain.xMin, value));
  };
  const dragBounds = dragRange
    ? {
        from: Math.min(dragRange.start, dragRange.current),
        to: Math.max(dragRange.start, dragRange.current),
      }
    : null;

  return (
    <div ref={hostRef} className="error-plot">
      {zoomDomain && resetLabel ? (
        <button type="button" className="error-plot-reset" onClick={resetZoom}>
          {resetLabel}
        </button>
      ) : null}
      {width > 0 && height > 0 ? (
        <svg width={width} height={height} role="img" aria-label={title}>
          <text className="error-plot-title" x={margin.left + innerWidth / 2} y={18}>
            {title}
          </text>

          {yTicks.map((tick) => (
            <g key={`y-${tick}`}>
              <line
                x1={margin.left}
                x2={margin.left + innerWidth}
                y1={yScale(tick)}
                y2={yScale(tick)}
                className="plot-grid-line"
                strokeWidth={1}
              />
              <text className="error-plot-tick" x={margin.left - 8} y={yScale(tick) + 3} textAnchor="end">
                {logScale ? powerLabel(tick) : tick.toPrecision(2)}
              </text>
            </g>
          ))}

          {xTicks.map((tick) => (
            <g key={`x-${tick}`}>
              <line
                x1={xScale(tick)}
                x2={xScale(tick)}
                y1={margin.top + innerHeight}
                y2={margin.top + innerHeight + 5}
                className="plot-axis-line"
                strokeWidth={1}
              />
              <text
                className="error-plot-tick"
                x={xScale(tick)}
                y={margin.top + innerHeight + 17}
                textAnchor="middle"
              >
                {formatTick(tick, xStep)}
              </text>
            </g>
          ))}

          <rect
            x={margin.left}
            y={margin.top}
            width={innerWidth}
            height={innerHeight}
            fill="none"
            className="plot-axis-line"
            strokeWidth={1}
          />

          {/* Series geometry is clipped to the plot frame: the visible-window
              slice keeps one point outside each edge, and those (plus zoomed
              line segments) must not paint over the axes. */}
          <clipPath id={clipId}>
            <rect x={margin.left} y={margin.top} width={innerWidth} height={innerHeight} />
          </clipPath>

          {/* Drag-to-zoom surface: sits under the markers so marker clicks
              still work; a horizontal drag zooms the X range. */}
          <rect
            x={margin.left}
            y={margin.top}
            width={innerWidth}
            height={innerHeight}
            fill="transparent"
            className="error-plot-zoom-surface"
            onPointerDown={(event) => {
              event.currentTarget.setPointerCapture(event.pointerId);
              const x = toDataX(event);
              setDragRange({ start: x, current: x });
            }}
            onPointerMove={(event) => {
              if (dragRange) setDragRange({ ...dragRange, current: toDataX(event) });
            }}
            onPointerUp={() => {
              if (!dragRange || !dragBounds) return;
              const span = domain.xMax - domain.xMin;
              if (dragBounds.to - dragBounds.from > span * 0.005) {
                const next = { xMin: dragBounds.from, xMax: dragBounds.to };
                setZoomDomain(next);
                onZoomRangeChange?.(next);
              }
              setDragRange(null);
            }}
            onDoubleClick={resetZoom}
          />

          {dragBounds && dragBounds.to - dragBounds.from > 1e-9 ? (
            <rect
              className="error-plot-zoom-selection"
              x={xScale(dragBounds.from)}
              y={margin.top}
              width={xScale(dragBounds.to) - xScale(dragBounds.from)}
              height={innerHeight}
            />
          ) : null}

          {showThreshold ? (
            <g>
              <line
                x1={margin.left}
                x2={margin.left + innerWidth}
                y1={thresholdY}
                y2={thresholdY}
                className="plot-threshold-line"
                strokeWidth={1}
                strokeDasharray="5 4"
              />
              <text
                className="error-plot-threshold-label"
                x={margin.left + innerWidth - 4}
                y={thresholdY - 4}
                textAnchor="end"
              >
                {thresholdLabel}
              </text>
            </g>
          ) : null}

          {selectedPixelX != null && selectedPixelX >= margin.left && selectedPixelX <= margin.left + innerWidth ? (
            <line
              x1={selectedPixelX}
              x2={selectedPixelX}
              y1={margin.top}
              y2={margin.top + innerHeight}
              className="plot-selected-line"
              strokeWidth={1}
              strokeDasharray="3 3"
            />
          ) : null}

          {seriesGeometry.map(({ runId, color, lineWidth, path, markerStride, markers }) => (
            <g key={runId} clipPath={`url(#${clipId})`}>
              <path d={path} fill="none" stroke={color} strokeWidth={lineWidth} />
              {markers.map(({ point, cx, cy }, index) => {
                const isBest = bestKey === point.key;
                const isSelected = samePlotX(selectedX, point.x);
                const isValley = !isBest && !isSelected && (valleyKeys?.has(point.key) ?? false);
                if (index % markerStride !== 0 && !isBest && !isSelected && !isValley) return null;
                return (
                  <circle
                    key={point.key}
                    cx={cx}
                    cy={cy}
                    r={isBest || isSelected ? 4.5 : isValley ? 5 : 3}
                    fill={isValley ? "none" : color}
                    stroke={isSelected || isBest ? undefined : isValley ? color : "none"}
                    strokeWidth={isBest || isSelected || isValley ? 1.6 : 0}
                    className={`error-plot-marker${isSelected ? " is-selected" : isBest ? " is-best" : ""}`}
                    onClick={() => onSelect?.(point.x)}
                  >
                    <title>{`${point.label ? `${point.label} · ` : ""}${point.x}: ${point.metric.toExponential(3)}`}</title>
                  </circle>
                );
              })}
            </g>
          ))}

          <text
            className="error-plot-axis-label"
            x={margin.left + innerWidth / 2}
            y={height - 8}
            textAnchor="middle"
          >
            {xAxisLabel}
          </text>
          <text
            className="error-plot-axis-label"
            x={14}
            y={margin.top + innerHeight / 2}
            textAnchor="middle"
            transform={`rotate(-90 14 ${margin.top + innerHeight / 2})`}
          >
            {yAxisLabel}
          </text>
        </svg>
      ) : null}
    </div>
  );
}
