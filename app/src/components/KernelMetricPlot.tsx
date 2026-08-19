import { useEffect, useMemo, useState } from "react";
import type { Translator } from "../i18n";
import {
  compareKernelResultRows,
  type KernelResultRow,
} from "../engine/kernelRunGroup";
import {
  KERNEL_PLOT_MARGIN as MARGIN,
  kernelPlotDensity,
} from "../engine/kernelPlotDensity";
import { useElementSize } from "../hooks/useElementSize";
import { plotSeriesColor } from "./ErrorLinePlot";

const MAX_CATEGORIES = 120;
const PLOT_HEIGHT = 300;

function kernelCategoryKey(row: KernelResultRow): string {
  const parameters = Object.entries(row.parameters)
    .sort(([a], [b]) => a.localeCompare(b, undefined, { numeric: true, sensitivity: "base" }))
    .map(([key, value]) => `${key}=${String(value)}`)
    .join("|");
  return `${row.kernelId}|${parameters}`;
}

function kernelTickLines(row: KernelResultRow): string[] {
  const parameters = Object.entries(row.parameters)
    .sort(([a], [b]) => a.localeCompare(b, undefined, { numeric: true, sensitivity: "base" }))
    .map(([key, value]) => `${key}=${String(value)}`);
  return [row.kernelId, ...parameters];
}

function seriesPath(
  categories: KernelResultRow[],
  byCategory: Map<string, KernelResultRow>,
  x: (index: number) => number,
  y: (metric: number) => number,
): string {
  let drawing = false;
  return categories.map((category, index) => {
    const point = byCategory.get(kernelCategoryKey(category));
    if (!point) {
      drawing = false;
      return "";
    }
    const command = drawing ? "L" : "M";
    drawing = true;
    return `${command}${x(index).toFixed(2)},${y(point.metric).toFixed(2)}`;
  }).filter(Boolean).join(" ");
}

/** Categorical, log-scale line plot for Algorithm Test results. */
export function KernelMetricPlot({
  t,
  rows,
  selectedKey,
  onSelect,
}: {
  t: Translator;
  rows: KernelResultRow[];
  selectedKey?: string | null;
  onSelect?: (key: string) => void;
}) {
  const [zoomRange, setZoomRange] = useState<{ start: number; end: number } | null>(null);
  const [dragRange, setDragRange] = useState<{ start: number; current: number } | null>(null);
  const [hostRef, hostSize] = useElementSize();
  const model = useMemo(() => {
    const categoryByKey = new Map<string, KernelResultRow>();
    for (const row of rows) {
      const key = kernelCategoryKey(row);
      if (!categoryByKey.has(key)) categoryByKey.set(key, row);
    }
    const allCategories = [...categoryByKey.values()].sort(compareKernelResultRows);
    const categories = allCategories.slice(0, MAX_CATEGORIES);
    const categoryKeys = new Set(categories.map(kernelCategoryKey));
    const seriesByRun = new Map<string, {
      runId: string;
      label: string;
      byCategory: Map<string, KernelResultRow>;
    }>();
    for (const row of rows) {
      const categoryKey = kernelCategoryKey(row);
      if (!categoryKeys.has(categoryKey)) continue;
      const series = seriesByRun.get(row.runId) ?? {
        runId: row.runId,
        label: `${row.sampleLabel} / ${row.runId.slice(0, 8)}`,
        byCategory: new Map<string, KernelResultRow>(),
      };
      series.byCategory.set(categoryKey, row);
      seriesByRun.set(row.runId, series);
    }
    return {
      categories,
      series: [...seriesByRun.values()],
      truncated: allCategories.length > categories.length,
    };
  }, [rows]);

  // A filtered result set can change the category indices; an old categorical
  // range would otherwise select a different kernel after the data changes.
  useEffect(() => {
    setZoomRange(null);
    setDragRange(null);
  }, [model.categories]);

  if (model.categories.length === 0) return null;

  const visibleStart = zoomRange
    ? Math.min(zoomRange.start, model.categories.length - 1)
    : 0;
  const visibleEnd = zoomRange
    ? Math.min(Math.max(visibleStart, zoomRange.end), model.categories.length - 1)
    : model.categories.length - 1;
  const visibleCategories = model.categories.slice(visibleStart, visibleEnd + 1);
  // Density adapts to the host width: spacing compresses and tick labels thin
  // out, but every point stays drawn, hoverable, and clickable.
  const { width, labelEvery, pointRadius } = kernelPlotDensity(
    visibleCategories.length,
    hostSize.width > 0 ? hostSize.width : null,
  );
  const innerWidth = width - MARGIN.left - MARGIN.right;
  const innerHeight = PLOT_HEIGHT - MARGIN.top - MARGIN.bottom;
  const visibleKeys = new Set(visibleCategories.map(kernelCategoryKey));
  const metrics = model.series
    .flatMap((series) => [...series.byCategory.values()])
    .filter((row) => visibleKeys.has(kernelCategoryKey(row)) && Number.isFinite(row.metric))
    .map((row) => Math.max(row.metric, 1e-12));
  const minLog = Math.floor(Math.min(...metrics.map(Math.log10)));
  const rawMaxLog = Math.ceil(Math.max(...metrics.map(Math.log10)));
  const maxLog = rawMaxLog <= minLog ? minLog + 1 : rawMaxLog;
  const yTickStep = maxLog - minLog > 8 ? 2 : 1;
  const yTicks: number[] = [];
  for (let tick = minLog; tick <= maxLog; tick += yTickStep) yTicks.push(tick);

  const x = (index: number) =>
    visibleCategories.length === 1
      ? MARGIN.left + innerWidth / 2
      : MARGIN.left + (index / (visibleCategories.length - 1)) * innerWidth;
  const y = (metric: number) => {
    const logValue = Math.log10(Math.max(metric, 1e-12));
    return MARGIN.top + (1 - (logValue - minLog) / (maxLog - minLog)) * innerHeight;
  };
  const categoryIndexFromEvent = (event: { clientX: number; currentTarget: Element }) => {
    const rect = event.currentTarget.getBoundingClientRect();
    const ratio = (event.clientX - rect.left) / Math.max(1, rect.width);
    return Math.max(
      0,
      Math.min(visibleCategories.length - 1, Math.round(ratio * Math.max(0, visibleCategories.length - 1))),
    );
  };
  const dragBounds = dragRange
    ? {
        from: Math.min(dragRange.start, dragRange.current),
        to: Math.max(dragRange.start, dragRange.current),
      }
    : null;

  return (
    <section className="kernel-metric-plot" aria-label={t("analyze.k.metricPlotTitle")}>
      <div className="kernel-metric-plot-heading">
        <h3>{t("analyze.k.metricPlotTitle")}</h3>
        <span>{t("analyze.k.metricPlotHint")}</span>
      </div>
      {model.series.length > 1 ? (
        <div className="kernel-metric-legend">
          {model.series.map((series, index) => (
            <span className="kernel-metric-legend-item" key={series.runId}>
              <span className="swatch" style={{ background: plotSeriesColor(index) }} />
              {series.label}
            </span>
          ))}
        </div>
      ) : null}
      <div className="kernel-metric-chart-scroll" ref={hostRef}>
        {zoomRange ? (
          <button
            type="button"
            className="kernel-metric-reset"
            onClick={() => setZoomRange(null)}
          >
            {t("plot.resetZoom")}
          </button>
        ) : null}
        <svg
          className="kernel-metric-chart"
          width={width}
          height={PLOT_HEIGHT}
          role="img"
          aria-label={t("analyze.k.metricPlotTitle")}
        >
          {yTicks.map((tick) => {
            const tickY = y(10 ** tick);
            return (
              <g key={tick}>
                <line
                  x1={MARGIN.left}
                  x2={MARGIN.left + innerWidth}
                  y1={tickY}
                  y2={tickY}
                  className="kernel-metric-grid"
                />
                <text
                  x={MARGIN.left - 8}
                  y={tickY + 3}
                  textAnchor="end"
                  className="kernel-metric-tick"
                >
                  {`10^${tick}`}
                </text>
              </g>
            );
          })}

          <rect
            x={MARGIN.left}
            y={MARGIN.top}
            width={innerWidth}
            height={innerHeight}
            className="kernel-metric-frame"
          />

          {visibleCategories.map((category, index) => (index % labelEvery !== 0 ? null : (
            <g key={kernelCategoryKey(category)}>
              <line
                x1={x(index)}
                x2={x(index)}
                y1={MARGIN.top + innerHeight}
                y2={MARGIN.top + innerHeight + 5}
                className="kernel-metric-axis"
              />
              <text
                x={x(index)}
                y={MARGIN.top + innerHeight + 17}
                textAnchor="middle"
                className="kernel-metric-tick kernel-metric-kernel-label"
              >
                {kernelTickLines(category).map((line, lineIndex) => (
                  <tspan key={line} x={x(index)} dy={lineIndex === 0 ? 0 : 12}>
                    {line}
                  </tspan>
                ))}
              </text>
            </g>
          )))}

          <rect
            x={MARGIN.left}
            y={MARGIN.top}
            width={innerWidth}
            height={innerHeight}
            fill="transparent"
            className="kernel-metric-zoom-surface"
            onPointerDown={(event) => {
              event.currentTarget.setPointerCapture(event.pointerId);
              const index = categoryIndexFromEvent(event);
              setDragRange({ start: index, current: index });
            }}
            onPointerMove={(event) => {
              if (!dragRange) return;
              setDragRange({ ...dragRange, current: categoryIndexFromEvent(event) });
            }}
            onPointerUp={() => {
              if (!dragRange) return;
              const from = Math.min(dragRange.start, dragRange.current);
              const to = Math.max(dragRange.start, dragRange.current);
              if (to - from >= 1) {
                const base = zoomRange?.start ?? 0;
                setZoomRange({ start: base + from, end: base + to });
              }
              setDragRange(null);
            }}
            onPointerCancel={() => setDragRange(null)}
            onDoubleClick={() => {
              setZoomRange(null);
              setDragRange(null);
            }}
          />

          {dragBounds && dragBounds.to > dragBounds.from ? (
            <rect
              x={x(dragBounds.from)}
              y={MARGIN.top}
              width={Math.max(1, x(dragBounds.to) - x(dragBounds.from))}
              height={innerHeight}
              className="kernel-metric-zoom-selection"
            />
          ) : null}

          {model.series.map((series, seriesIndex) => {
            const color = plotSeriesColor(seriesIndex);
            return (
              <g key={series.runId}>
                <path
                  d={seriesPath(visibleCategories, series.byCategory, x, y)}
                  fill="none"
                  stroke={color}
                  strokeWidth={1.6}
                />
                {visibleCategories.map((category, index) => {
                  const point = series.byCategory.get(kernelCategoryKey(category));
                  if (!point) return null;
                  const key = `${point.runId}-${point.kernelLabel}`;
                  const selected = selectedKey === key;
                  return (
                    <circle
                      key={key}
                      cx={x(index)}
                      cy={y(point.metric)}
                      r={selected ? pointRadius + 1.5 : pointRadius}
                      fill={color}
                      stroke={selected ? undefined : "none"}
                      strokeWidth={selected ? 1.6 : 0}
                      className={`kernel-metric-point${selected ? " is-selected" : ""}`}
                      onClick={() => onSelect?.(key)}
                    >
                      <title>{`${point.kernelLabel} / ${point.sampleLabel}: ${point.metric.toExponential(3)}`}</title>
                    </circle>
                  );
                })}
              </g>
            );
          })}

          <text
            x={MARGIN.left + innerWidth / 2}
            y={PLOT_HEIGHT - 8}
            textAnchor="middle"
            className="kernel-metric-axis-label"
          >
            {t("analyze.col.kernel")}
          </text>
          <text
            x={14}
            y={MARGIN.top + innerHeight / 2}
            textAnchor="middle"
            transform={`rotate(-90 14 ${MARGIN.top + innerHeight / 2})`}
            className="kernel-metric-axis-label"
          >
            {t("analyze.axisRelativeError")}
          </text>
        </svg>
      </div>
      {model.truncated ? (
        <p className="help-copy">
          {t("analyze.k.metricPlotTruncated", { count: String(model.categories.length) })}
        </p>
      ) : null}
    </section>
  );
}
