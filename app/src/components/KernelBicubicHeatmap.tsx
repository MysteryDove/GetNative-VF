import { useState } from "react";
import type { Translator } from "../i18n";
import {
  bicubicCellKey,
  heatStep,
  type BicubicCell,
  type BicubicGrid,
} from "../engine/kernelHeatmap";
import { formatParameterValue } from "../engine/numberInput";
import { useElementSize } from "../hooks/useElementSize";

const HEAT_STEPS = 7;
const MARGIN = { top: 8, right: 10, bottom: 34, left: 46 } as const;
const GAP = 2;

/**
 * Bicubic (b, c) grid as a heatmap: c across, b down, cell shade = relative
 * error on a log scale (one hue; the lowest error recedes toward the surface,
 * so it is the lightest step on the light theme and the darkest on dark). The
 * lowest-error cell is ringed and labelled, hovering or focusing a cell reads
 * out its exact value, and clicking selects that kernel like a table row.
 */
export function KernelBicubicHeatmap({
  t,
  grid,
  selectedKey,
  onSelect,
}: {
  t: Translator;
  grid: BicubicGrid;
  selectedKey?: string | null;
  onSelect?: (key: string) => void;
}) {
  const [hostRef, hostSize] = useElementSize();
  const [hovered, setHovered] = useState<BicubicCell | null>(null);
  const columns = grid.cs.length;
  const rows = grid.bs.length;
  const available = (hostSize.width > 0 ? hostSize.width : 560) - MARGIN.left - MARGIN.right;
  const cell = Math.max(14, Math.min(44, Math.floor(available / columns)));
  const cellHeight = Math.max(14, Math.min(26, cell));
  const width = MARGIN.left + columns * cell + MARGIN.right;
  const height = MARGIN.top + rows * cellHeight + MARGIN.bottom;
  // Thin the axis labels rather than letting them collide.
  const columnEvery = Math.max(1, Math.ceil(30 / cell));
  const rowEvery = Math.max(1, Math.ceil(12 / cellHeight));
  const shown = hovered ?? grid.best;
  const describe = (item: BicubicCell) =>
    `b=${formatParameterValue(item.b)}, c=${formatParameterValue(item.c)}`;

  return (
    <section className="kernel-metric-plot kernel-heatmap" aria-label={t("analyze.k.heatmapTitle")}>
      <div className="kernel-metric-plot-heading">
        <h3>{t("analyze.k.heatmapTitle")}</h3>
        <span>{t("analyze.k.heatmapHint")}</span>
      </div>
      <div className="kernel-metric-chart-scroll" ref={hostRef}>
        <svg width={width} height={height} role="img" aria-label={t("analyze.k.heatmapTitle")}>
          {grid.bs.map((b, rowIndex) =>
            grid.cs.map((c, columnIndex) => {
              const item = grid.cells.get(bicubicCellKey(b, c));
              const x = MARGIN.left + columnIndex * cell;
              const y = MARGIN.top + rowIndex * cellHeight;
              if (!item) {
                return (
                  <rect
                    key={`${b}|${c}`}
                    className="kernel-heatmap-empty"
                    x={x + GAP / 2}
                    y={y + GAP / 2}
                    width={cell - GAP}
                    height={cellHeight - GAP}
                    rx={2}
                  />
                );
              }
              const selected = selectedKey != null && item.keys.includes(selectedKey);
              const best = item === grid.best;
              return (
                <rect
                  key={`${b}|${c}`}
                  className={`kernel-heatmap-cell heat-${heatStep(item.metric, grid.min, grid.max, HEAT_STEPS)}`
                    + `${best ? " is-best" : ""}${selected ? " is-selected" : ""}`}
                  x={x + GAP / 2}
                  y={y + GAP / 2}
                  width={cell - GAP}
                  height={cellHeight - GAP}
                  rx={2}
                  tabIndex={0}
                  role="button"
                  aria-label={`${describe(item)}: ${item.metric.toExponential(2)}`}
                  aria-pressed={selected}
                  onPointerEnter={() => setHovered(item)}
                  onPointerLeave={() => setHovered(null)}
                  onFocus={() => setHovered(item)}
                  onBlur={() => setHovered(null)}
                  onClick={() => onSelect?.(item.keys[0])}
                  onKeyDown={(event) => {
                    if (event.key === "Enter" || event.key === " ") {
                      event.preventDefault();
                      onSelect?.(item.keys[0]);
                    }
                  }}
                />
              );
            }),
          )}
          {grid.bs.map((b, rowIndex) => (rowIndex % rowEvery !== 0 ? null : (
            <text
              key={b}
              className="kernel-metric-tick"
              x={MARGIN.left - 6}
              y={MARGIN.top + rowIndex * cellHeight + cellHeight / 2 + 3}
              textAnchor="end"
            >
              {formatParameterValue(b)}
            </text>
          )))}
          {grid.cs.map((c, columnIndex) => (columnIndex % columnEvery !== 0 ? null : (
            <text
              key={c}
              className="kernel-metric-tick"
              x={MARGIN.left + columnIndex * cell + cell / 2}
              y={MARGIN.top + rows * cellHeight + 13}
              textAnchor="middle"
            >
              {formatParameterValue(c)}
            </text>
          )))}
          <text
            className="kernel-metric-axis-label"
            x={MARGIN.left + (columns * cell) / 2}
            y={height - 4}
            textAnchor="middle"
          >
            c
          </text>
          <text
            className="kernel-metric-axis-label"
            x={12}
            y={MARGIN.top + (rows * cellHeight) / 2 + 3}
            textAnchor="middle"
          >
            b
          </text>
        </svg>
      </div>
      <div className="kernel-heatmap-footer">
        <span className="kernel-heatmap-readout" role="status">
          <strong>{hovered ? describe(shown) : `${t("analyze.k.heatmapBest")} · ${describe(shown)}`}</strong>
          {` ${shown.metric.toExponential(2)}`}
          {shown.count > 1 ? ` · ${t("analyze.k.heatmapMean", { count: String(shown.count) })}` : ""}
        </span>
        <span className="kernel-heatmap-scale" aria-label={t("analyze.k.heatmapScale")}>
          <span>{grid.min.toExponential(1)}</span>
          {Array.from({ length: HEAT_STEPS }, (_, step) => (
            <i key={step} className={`heat-${step}`} />
          ))}
          <span>{grid.max.toExponential(1)}</span>
          <span className="kernel-heatmap-scale-label">{t("analyze.k.heatmapScale")}</span>
        </span>
      </div>
    </section>
  );
}
