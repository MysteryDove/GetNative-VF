import { useEffect, useMemo, useRef, useState } from "react";

export type ErrorPlotDatum = {
  key: string;
  runId: string;
  /** X position as a numeric string (height candidate, frame index, …). */
  x: string;
  metric: number;
  color: string;
  /** Series label shown in the marker tooltip, e.g. "8bit.png · Lanczos 3". */
  label?: string;
};

const AXIS_COLOR = "#4a514d";

/** Fallback series color when a datum carries no explicit color. */
export const DEFAULT_SERIES_COLOR = "#3b82f6";

/** Shared plot series palette (analyze plot, verify plot, sample swatches). */
const SERIES_PALETTE = [DEFAULT_SERIES_COLOR, "#22c55e", "#f59e0b", "#a855f7", "#ef4444", "#06b6d4"];

export function plotSeriesColor(index: number): string {
  return SERIES_PALETTE[index % SERIES_PALETTE.length];
}const GRID_COLOR = "#232826";
const THRESHOLD_COLOR = "#c9a24b";
const SELECTED_COLOR = "#6ea8fe";

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
 * descale answer, unlike naive every-Nth sampling.
 */
function decimateMinMax<T>(points: T[], value: (point: T) => number, maxBuckets: number): T[] {
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
const MAX_MARKERS = 300;

function useElementSize() {
  const ref = useRef<HTMLDivElement | null>(null);
  const [size, setSize] = useState({ width: 0, height: 0 });
  useEffect(() => {
    const element = ref.current;
    if (!element) return;
    const observer = new ResizeObserver((entries) => {
      const box = entries[0]?.contentRect;
      if (box) setSize({ width: box.width, height: box.height });
    });
    observer.observe(element);
    return () => observer.disconnect();
  }, []);
  return [ref, size] as const;
}

/**
 * getnative-style relative-error plot: one line+marker series per run,
 * numeric X, Relative error on (log) Y, dashed 1e-6 perfect-descale guide.
 * Drag across the plot to zoom into an X range (reported via
 * onZoomRangeChange); double-click or the reset button zooms back out.
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
    const xs = flat.map((point) => Number(point.x));
    let xMin = Math.min(...xs);
    let xMax = Math.max(...xs);
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
      const logs = yPoints.map((point) => Math.log10(point.metric + 1e-9));
      const yMin = Math.floor(Math.min(...logs, Math.log10(threshold)));
      const yMax = Math.ceil(Math.max(...logs));
      return { xMin, xMax, yMin, yMax: yMax <= yMin ? yMin + 1 : yMax };
    }
    const yMax = Math.max(...yPoints.map((point) => point.metric), threshold) * 1.05;
    return { xMin, xMax, yMin: 0, yMax };
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

  if (!domain) return <div ref={hostRef} className="error-plot" />;

  const xScale = (value: number) =>
    margin.left + ((value - domain.xMin) / (domain.xMax - domain.xMin)) * innerWidth;
  const yValue = (metric: number) =>
    logScale ? Math.log10(metric + 1e-9) : metric;
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
    const rect = event.currentTarget.getBoundingClientRect();
    const ratio = (event.clientX - rect.left - margin.left) / Math.max(1, innerWidth);
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
                stroke={GRID_COLOR}
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
                stroke={AXIS_COLOR}
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
            stroke={AXIS_COLOR}
            strokeWidth={1}
          />

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
                stroke={THRESHOLD_COLOR}
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
              stroke={SELECTED_COLOR}
              strokeWidth={1}
              strokeDasharray="3 3"
            />
          ) : null}

          {series.map(({ runId, points }) => {
            const color = points[0]?.color ?? DEFAULT_SERIES_COLOR;
            const pathPoints = decimateMinMax(
              points,
              (point) => yValue(point.metric),
              Math.max(64, Math.floor(innerWidth)),
            );
            const path = pathPoints
              .map(
                (point, index) =>
                  `${index === 0 ? "M" : "L"}${xScale(Number(point.x)).toFixed(2)},${yScale(yValue(point.metric)).toFixed(2)}`,
              )
              .join(" ");
            const markerStride = Math.max(1, Math.ceil(points.length / MAX_MARKERS));
            return (
              <g key={runId}>
                <path d={path} fill="none" stroke={color} strokeWidth={1.6} />
                {points.map((point, index) => {
                  const isBest = bestKey === point.key;
                  const isSelected = selectedX === point.x;
                  if (index % markerStride !== 0 && !isBest && !isSelected) return null;
                  return (
                    <circle
                      key={point.key}
                      cx={xScale(Number(point.x))}
                      cy={yScale(yValue(point.metric))}
                      r={isBest || isSelected ? 4.5 : 3}
                      fill={color}
                      stroke={isSelected ? SELECTED_COLOR : isBest ? "#f0f4f2" : "none"}
                      strokeWidth={isBest || isSelected ? 1.6 : 0}
                      className="error-plot-marker"
                      onClick={() => onSelect?.(point.x)}
                    >
                      <title>{`${point.label ? `${point.label} · ` : ""}${point.x}: ${point.metric.toExponential(3)}`}</title>
                    </circle>
                  );
                })}
              </g>
            );
          })}

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
