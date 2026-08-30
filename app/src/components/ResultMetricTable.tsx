import { useMemo, useState, type ReactNode } from "react";
import { ArrowDown, ArrowDownUp, ArrowUp } from "lucide-react";
import { PERFECTLY_DESCALE_THRESHOLD } from "../engine/valleyDetect";
import type { Translator } from "../i18n";

export { PERFECTLY_DESCALE_THRESHOLD };

const TABLE_ROW_HEIGHT = 28;
// Covers the 38vh container plus overscan slack.
const TABLE_VIEWPORT = 480;

export type ResultMetricTableRow = {
  key: string;
  metric: number;
  /** Cell contents in column order, excluding the metric column. */
  cells: ReactNode[];
  selected?: boolean;
  onSelect?: () => void;
};

/**
 * Shared error-metric result table for the Analyze pages: exponential error
 * display, click-to-sort metric column, perfect-descale highlighting, and
 * windowed rendering for 100k+ row grids.
 */
export function ResultMetricTable({
  t,
  ariaLabel,
  columns,
  metricColumnIndex,
  columnTemplate,
  rows,
}: {
  t: Translator;
  ariaLabel: string;
  /** Header labels including the metric column. */
  columns: string[];
  metricColumnIndex: number;
  /** grid-template-columns for head and rows. */
  columnTemplate: string;
  rows: ResultMetricTableRow[];
}) {
  const [metricSort, setMetricSort] = useState<"none" | "asc" | "desc">("none");
  const [scrollTop, setScrollTop] = useState(0);

  const sortedRows = useMemo(() => {
    if (metricSort === "none") return rows;
    const factor = metricSort === "asc" ? 1 : -1;
    return [...rows].sort((a, b) => (a.metric - b.metric) * factor);
  }, [rows, metricSort]);

  const tableWindow = useMemo(() => {
    const start = Math.max(0, Math.floor(scrollTop / TABLE_ROW_HEIGHT) - 10);
    const end = Math.min(
      sortedRows.length,
      Math.ceil((scrollTop + TABLE_VIEWPORT) / TABLE_ROW_HEIGHT) + 10,
    );
    return { start, end };
  }, [scrollTop, sortedRows.length]);

  const gridStyle = { gridTemplateColumns: columnTemplate };

  return (
    <div
      className="result-table"
      role="table"
      aria-label={ariaLabel}
      onScroll={(event) => setScrollTop(event.currentTarget.scrollTop)}
    >
      <div className="result-table-head" role="row" style={gridStyle}>
        {columns.map((label, index) =>
          index === metricColumnIndex ? (
            <button
              key={index}
              type="button"
              role="columnheader"
              className={`result-table-sort ${metricSort !== "none" ? "active" : ""}`}
              title={t("analyze.sortByMetric")}
              aria-label={t("analyze.sortByMetric")}
              aria-sort={metricSort === "none" ? "none" : metricSort === "asc" ? "ascending" : "descending"}
              onClick={() =>
                setMetricSort((current) =>
                  current === "none" ? "asc" : current === "asc" ? "desc" : "none",
                )
              }
            >
              {label}
              <span className="result-table-sort-icon" aria-hidden="true">
                {metricSort === "asc" ? <ArrowUp size={13} /> : metricSort === "desc" ? <ArrowDown size={13} /> : <ArrowDownUp size={13} />}
              </span>
            </button>
          ) : (
            <span key={index} role="columnheader">
              {label}
            </span>
          ),
        )}
      </div>
      <div style={{ height: tableWindow.start * TABLE_ROW_HEIGHT }} aria-hidden="true" />
      {sortedRows.slice(tableWindow.start, tableWindow.end).map((row) => {
        const className = [
          "result-table-row",
          row.selected ? "selected" : "",
          row.metric <= PERFECTLY_DESCALE_THRESHOLD ? "perfect" : "",
        ].join(" ");
        const cells = [
          ...row.cells.slice(0, metricColumnIndex),
          Number.isFinite(row.metric) ? row.metric.toExponential(2) : "—",
          ...row.cells.slice(metricColumnIndex),
        ].map((cell, index) => (
          <span role="cell" key={index}>
            {cell}
          </span>
        ));
        return row.onSelect ? (
          <button
            type="button"
            className={className}
            role="row"
            key={row.key}
            style={gridStyle}
            onClick={row.onSelect}
          >
            {cells}
          </button>
        ) : (
          <div className={className} role="row" key={row.key} style={gridStyle}>
            {cells}
          </div>
        );
      })}
      <div
        style={{ height: (sortedRows.length - tableWindow.end) * TABLE_ROW_HEIGHT }}
        aria-hidden="true"
      />
    </div>
  );
}
