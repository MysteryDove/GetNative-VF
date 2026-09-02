import { useMemo, useRef, useState, type ReactNode, type UIEvent } from "react";
import { ArrowDown, ArrowDownUp, ArrowUp } from "lucide-react";
import { PERFECTLY_DESCALE_THRESHOLD } from "../engine/valleyDetect";
import type { Translator } from "../i18n";

export { PERFECTLY_DESCALE_THRESHOLD };

const TABLE_ROW_HEIGHT = 28;
// Covers the 38vh container plus overscan slack.
const TABLE_VIEWPORT = 480;
const TABLE_OVERSCAN = 10;

function tableWindowFor(scrollTop: number, length: number): { start: number; end: number } {
  const start = Math.max(0, Math.floor(scrollTop / TABLE_ROW_HEIGHT) - TABLE_OVERSCAN);
  const end = Math.min(
    length,
    Math.ceil((scrollTop + TABLE_VIEWPORT) / TABLE_ROW_HEIGHT) + TABLE_OVERSCAN,
  );
  return { start, end };
}

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
  defaultMetricSort = "none",
  limit,
  scrollable = true,
}: {
  t: Translator;
  ariaLabel: string;
  /** Header labels including the metric column. */
  columns: string[];
  metricColumnIndex: number;
  /** grid-template-columns for head and rows. */
  columnTemplate: string;
  rows: ResultMetricTableRow[];
  defaultMetricSort?: "none" | "asc" | "desc";
  /** After sorting, keep only the first N rows. */
  limit?: number;
  scrollable?: boolean;
}) {
  const [metricSort, setMetricSort] = useState<"none" | "asc" | "desc">(defaultMetricSort);
  const [scrollTop, setScrollTop] = useState(0);
  const windowRef = useRef({ start: 0, end: 0 });

  const sortedRows = useMemo(() => {
    const ordered = metricSort === "none"
      ? rows
      : [...rows].sort((a, b) => (a.metric - b.metric) * (metricSort === "asc" ? 1 : -1));
    return limit != null ? ordered.slice(0, limit) : ordered;
  }, [rows, metricSort, limit]);

  const windowed = scrollable;
  const tableWindow = useMemo(
    () => windowed ? tableWindowFor(scrollTop, sortedRows.length) : { start: 0, end: sortedRows.length },
    [scrollTop, sortedRows.length, windowed],
  );
  windowRef.current = tableWindow;

  function handleScroll(event: UIEvent<HTMLDivElement>) {
    if (!windowed) return;
    const next = tableWindowFor(event.currentTarget.scrollTop, sortedRows.length);
    const prev = windowRef.current;
    if (next.start === prev.start && next.end === prev.end) return;
    setScrollTop(event.currentTarget.scrollTop);
  }

  const gridStyle = { gridTemplateColumns: columnTemplate };

  return (
    <div
      className={`result-table${windowed ? "" : " result-table-static"}`}
      role="table"
      aria-label={ariaLabel}
      onScroll={windowed ? handleScroll : undefined}
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
      <div
        className="result-table-spacer"
        style={{ height: tableWindow.start * TABLE_ROW_HEIGHT }}
        aria-hidden="true"
      />
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
        className="result-table-spacer"
        style={{ height: (sortedRows.length - tableWindow.end) * TABLE_ROW_HEIGHT }}
        aria-hidden="true"
      />
    </div>
  );
}
