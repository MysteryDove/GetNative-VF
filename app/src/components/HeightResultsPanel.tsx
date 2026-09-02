import { useEffect, useMemo, useRef, useState } from "react";
import { ChevronDown, ChevronRight } from "lucide-react";
import type { Translator } from "../i18n";
import type { ProjectState } from "../project/types";
import type { estimateHeightWork, resolveHeightGrid } from "../engine/heightDraft";
import { kernelMetaLabel, type SeriesTable } from "../engine/runGroupPlan";
import { toggleSetValue } from "../utils/collections";
import { BlockedState } from "./BlockedState";
import {
  ErrorLinePlot,
  plotSeriesColor,
  DEFAULT_SERIES_COLOR,
  type ErrorPlotDatum,
} from "./ErrorLinePlot";
import {
  PERFECTLY_DESCALE_THRESHOLD,
  ResultMetricTable,
} from "./ResultMetricTable";
import { detectValleys, type ValleySeries } from "../engine/valleyDetect";
import { sourceFilterLabel } from "../project/sourceLabel";

const HEIGHT_TABLE_LIMIT = 20;

/** Display a measured height with up to two decimals, trimming zeros. */
function formatHeight(value: number): string {
  return Number(value.toFixed(2)).toString();
}

/** Heights come from mixed sources (grid draft, engine rows); compare numerically. */
function sameHeight(a: string | null, b: string): boolean {
  if (a == null) return false;
  const na = Number(a);
  const nb = Number(b);
  if (!Number.isFinite(na) || !Number.isFinite(nb)) return a === b;
  return Math.abs(na - nb) <= 1e-6 * Math.max(1, Math.abs(na), Math.abs(nb));
}

export function filterHeightSeriesRunIds(
  series: Array<{ runId: string; sampleId: string }>,
  state: Pick<ProjectState, "runsById" | "samplesById">,
  runGroupFilter: string,
  sourceFilter: string,
): Set<string> {
  const ids = new Set<string>();
  for (const item of series) {
    const run = state.runsById[item.runId];
    const groupValue = run?.runGroupId ? run.runGroupId : `run:${item.runId}`;
    const sourceId = run?.sourceId || state.samplesById[item.sampleId]?.sourceId || "";
    if (runGroupFilter !== "all" && groupValue !== runGroupFilter) continue;
    if (sourceFilter !== "all" && sourceId !== sourceFilter) continue;
    ids.add(item.runId);
  }
  return ids;
}

/**
 * Height-scan results pane (center): plot with series legend and verdict,
 * collapsible result table with kernel-group filter, candidate preview when
 * no runs have landed yet.
 */
export function HeightResultsPanel({
  t,
  state,
  axisMode,
  analyzeAvailable,
  seriesRows,
  grid,
  work,
  plotTitle,
  hasIncludedSamples,
  applyBusy,
  applyNotice,
  submitNotice,
  showExcludedResults,
  excludedResultsAvailable,
  onToggleExcludedResults,
  onOpenDiagnostics,
  onOpenApplyDialog,
  onRefineAroundSelection,
}: {
  t: Translator;
  state: ProjectState;
  axisMode: "h_plus_w" | "h_only" | "w_only";
  analyzeAvailable: boolean;
  seriesRows: SeriesTable;
  grid: ReturnType<typeof resolveHeightGrid>;
  work: ReturnType<typeof estimateHeightWork>;
  plotTitle: string;
  hasIncludedSamples: boolean;
  applyBusy: boolean;
  applyNotice: string;
  submitNotice: string | null;
  showExcludedResults: boolean;
  /** False when no Sample is excluded; the toggle is hidden then. */
  excludedResultsAvailable: boolean;
  onToggleExcludedResults: (value: boolean) => void;
  onOpenDiagnostics: () => void;
  onOpenApplyDialog: (selected?: string) => void;
  onRefineAroundSelection: (height: string) => void;
}) {
  const [selectedHeight, setSelectedHeight] = useState<string | null>(null);
  const selectedHeightAxis = useRef<typeof axisMode>(axisMode);
  const [logDisplay, setLogDisplay] = useState(true);
  // Zoom window reported by the plot; also scopes the valley search (scope D).
  const [zoomRange, setZoomRange] = useState<{ xMin: number; xMax: number } | null>(null);
  // null = auto: expanded while empty (candidate preview), collapsed once results land.
  const [tableCollapsed, setTableCollapsed] = useState<boolean | null>(null);
  const [hiddenRunIds, setHiddenRunIds] = useState<Set<string>>(new Set());
  const [kernelFilter, setKernelFilter] = useState<string | null>(null);
  const [runGroupFilter, setRunGroupFilter] = useState("all");
  const [sourceFilter, setSourceFilter] = useState("all");

  useEffect(() => {
    // A selected candidate is axis-specific. Keeping a W-only selection when
    // switching to H/H+W would reinterpret that width as a height on Apply.
    setSelectedHeight(null);
    selectedHeightAxis.current = axisMode;
    setZoomRange(null);
  }, [axisMode]);

  function selectHeight(value: string | null) {
    selectedHeightAxis.current = axisMode;
    setSelectedHeight(value);
  }

  function toggleRunVisibility(runId: string) {
    setHiddenRunIds((current) => toggleSetValue(current, runId));
  }

  // One plot series per run (sample × kernel variant), each with a stable color.
  const allRunSeries = useMemo(
    () =>
      seriesRows.seriesMeta.map((meta, index) => ({
        ...meta,
        color: plotSeriesColor(index),
        label: `${meta.sampleLabel} · ${kernelMetaLabel(t, meta)}`,
      })),
    [seriesRows, t],
  );
  const runFilterOptions = useMemo(() => {
    const values = new Set<string>();
    for (const series of allRunSeries) {
      const run = state.runsById[series.runId];
      const value = run?.runGroupId ? run.runGroupId : `run:${series.runId}`;
      values.add(value);
    }
    const ordered = [...values].sort((a, b) => {
      const aTime = a.startsWith("run:")
        ? state.runsById[a.slice(4)]?.createdAt
        : state.runGroupsById[a]?.createdAt;
      const bTime = b.startsWith("run:")
        ? state.runsById[b.slice(4)]?.createdAt
        : state.runGroupsById[b]?.createdAt;
      return (aTime ?? "").localeCompare(bTime ?? "") || a.localeCompare(b);
    });
    return ordered.map((value, index) => ({
      value,
      label: t("results.runOption", { number: String(index + 1) }),
    }));
  }, [allRunSeries, state.runGroupsById, state.runsById, t]);
  const sourceFilterOptions = useMemo(() => {
    const values: string[] = [];
    for (const series of allRunSeries) {
      const run = state.runsById[series.runId];
      const sourceId = run?.sourceId || state.samplesById[series.sampleId]?.sourceId;
      if (sourceId && !values.includes(sourceId)) values.push(sourceId);
    }
    return values.map((value) => ({
      value,
      label: sourceFilterLabel(value, state),
    }));
  }, [allRunSeries, state]);
  const filteredRunIds = useMemo(() => {
    return filterHeightSeriesRunIds(
      allRunSeries,
      state,
      runGroupFilter,
      sourceFilter,
    );
  }, [allRunSeries, runGroupFilter, sourceFilter, state]);
  const runSeries = useMemo(
    () => allRunSeries.filter((series) => filteredRunIds.has(series.runId)),
    [allRunSeries, filteredRunIds],
  );
  const runColorById = useMemo(
    () => new Map(allRunSeries.map((series) => [series.runId, series.color])),
    [allRunSeries],
  );
  const runLabelById = useMemo(
    () => new Map(allRunSeries.map((series) => [series.runId, series.label])),
    [allRunSeries],
  );
  const runKernelById = useMemo(
    () =>
      new Map(
        seriesRows.seriesMeta.map((meta) => [meta.runId, kernelMetaLabel(t, meta)]),
      ),
    [seriesRows, t],
  );

  const plotData = useMemo<ErrorPlotDatum[]>(
    () =>
      seriesRows.rows
        .filter((point) => filteredRunIds.has(point.runId) && !hiddenRunIds.has(point.runId))
        .map((point) => ({
          key: `${point.runId}-${point.height}`,
          runId: point.runId,
          x: point.height,
          metric: point.metric,
          color: runColorById.get(point.runId) ?? DEFAULT_SERIES_COLOR,
          label: runLabelById.get(point.runId),
        })),
    [seriesRows, filteredRunIds, hiddenRunIds, runColorById, runLabelById],
  );

  // Reading aids for the getnative workflow: prominent local valleys per
  // series, clustered across runs for consensus (a global minimum would land
  // on the scan edge whenever the curve rides a monotonic trend), plus the
  // perfectly-descale threshold (error <= 1e-6). Scoped to the zoom window.
  const plotAids = useMemo(() => {
    if (!plotData.length) return null;
    const inRange = zoomRange
      ? plotData.filter(
          (point) => Number(point.x) >= zoomRange.xMin && Number(point.x) <= zoomRange.xMax,
        )
      : plotData;
    const scoped = inRange.length ? inRange : plotData;
    const byRun = new Map<string, ValleySeries["points"]>();
    for (const point of scoped) {
      const x = Number(point.x);
      if (!Number.isFinite(x) || !Number.isFinite(point.metric)) continue;
      const bucket = byRun.get(point.runId) ?? [];
      bucket.push({ x, metric: point.metric, key: point.key });
      byRun.set(point.runId, bucket);
    }
    const detection = detectValleys(
      [...byRun.entries()].map(([runId, points]) => ({ runId, points })),
    );
    const top = detection.candidates[0] ?? null;
    const best = top?.deepest ?? detection.fallback;
    if (!best) return null;
    return {
      bestKey: best.key ?? null,
      bestHeight: formatHeight(best.x),
      bestMetric: best.metric,
      bestKernel: runKernelById.get(best.runId) ?? "",
      perfectCount: scoped.filter(
        (point) => point.metric <= PERFECTLY_DESCALE_THRESHOLD,
      ).length,
      totalCount: scoped.length,
      candidates: detection.candidates.slice(0, 3),
      valleyKeys: new Set(
        detection.candidates
          .slice(0, 3)
          .flatMap((candidate) => candidate.members.map((member) => member.key))
          .filter((key): key is string => key != null),
      ),
      // True when only a trend-driven edge minimum exists — shown honestly.
      isEdgeFallback: top == null,
    };
  }, [plotData, zoomRange, runKernelById]);

  // Kernel group selector for the result table (only when runs differ by kernel).
  const kernelGroups = useMemo(() => {
    const seen = new Map<string, string>();
    for (const meta of seriesRows.seriesMeta) {
      if (!filteredRunIds.has(meta.runId)) continue;
      if (!seen.has(meta.kernelKey)) seen.set(meta.kernelKey, kernelMetaLabel(t, meta));
    }
    return [...seen.entries()].map(([key, label]) => ({ key, label }));
  }, [seriesRows, filteredRunIds, t]);

  const filteredRows = useMemo(
    () =>
      kernelFilter
        ? seriesRows.rows.filter(
            (row) => filteredRunIds.has(row.runId) && row.kernelKey === kernelFilter,
          )
        : seriesRows.rows.filter((row) => filteredRunIds.has(row.runId)),
    [seriesRows, kernelFilter, filteredRunIds],
  );

  const tableRows = useMemo(
    () =>
      filteredRows.map((row) => ({
        key: `${row.runId}-${row.height}`,
        metric: row.metric,
        cells: [row.height, row.sampleLabel, row.kernelId, row.runId.slice(0, 10)],
        selected: sameHeight(selectedHeight, row.height),
        onSelect: () => selectHeight(row.height),
      })),
    [filteredRows, selectedHeight],
  );

  const isTableCollapsed = tableCollapsed ?? tableRows.length > 0;

  return (
    <section className="analyze-plot pane">
      <div className="analyze-plot-toolbar">
        <label className="series-visibility">
          <input
            type="checkbox"
            checked={logDisplay}
            onChange={(event) => setLogDisplay(event.target.checked)}
          />
          <span>{t("analyze.logDisplay")}</span>
        </label>
        <button
          className="secondary-button"
          type="button"
          disabled={!selectedHeight}
          onClick={() => {
            if (selectedHeight) onRefineAroundSelection(selectedHeight);
          }}
        >
          {t("analyze.refineAroundSelection")}
        </button>
        <button
          className="secondary-button"
          type="button"
          disabled={applyBusy || !hasIncludedSamples}
          onClick={() => onOpenApplyDialog(
            selectedHeightAxis.current === axisMode ? selectedHeight ?? undefined : undefined,
          )}
        >
          {t("analyze.applyToRecipe")}
        </button>
        {applyNotice || submitNotice ? (
          <span className="help-copy">{applyNotice || submitNotice}</span>
        ) : null}
        {excludedResultsAvailable ? (
          <label className="series-visibility analyze-excluded-toggle">
            <input
              type="checkbox"
              checked={showExcludedResults}
              onChange={(event) => onToggleExcludedResults(event.target.checked)}
            />
            <span>{t("analyze.showExcludedResults")}</span>
          </label>
        ) : null}
      </div>
      {seriesRows.incompatibleCount > 0 ? (
        <p className="help-copy warning-copy">
          {t("analyze.incompatibleMetricHidden", {
            count: String(seriesRows.incompatibleCount),
          })}
        </p>
      ) : null}

      {allRunSeries.length > 0 ? (
        <div className="results-filters height-results-filters" aria-label={t("results.filters.title")}>
          <div className="results-filter-group" role="radiogroup" aria-label={t("results.filters.runs")}>
            <span className="results-filter-label">{t("results.filters.runs")}</span>
            <div className="button-radio">
              <button
                type="button"
                role="radio"
                aria-checked={runGroupFilter === "all"}
                className={runGroupFilter === "all" ? "active" : ""}
                onClick={() => setRunGroupFilter("all")}
              >
                {t("results.filters.all")}
              </button>
              {runFilterOptions.map((option) => (
                <button
                  key={option.value}
                  type="button"
                  role="radio"
                  aria-checked={runGroupFilter === option.value}
                  className={runGroupFilter === option.value ? "active" : ""}
                  onClick={() => setRunGroupFilter(option.value)}
                >
                  {option.label}
                </button>
              ))}
            </div>
          </div>
          <div className="results-filter-group" role="radiogroup" aria-label={t("results.filters.source")}>
            <span className="results-filter-label">{t("results.filters.source")}</span>
            <div className="button-radio">
              <button
                type="button"
                role="radio"
                aria-checked={sourceFilter === "all"}
                className={sourceFilter === "all" ? "active" : ""}
                onClick={() => setSourceFilter("all")}
              >
                {t("results.filters.all")}
              </button>
              {sourceFilterOptions.map((option) => (
                <button
                  key={option.value}
                  type="button"
                  role="radio"
                  aria-checked={sourceFilter === option.value}
                  className={sourceFilter === option.value ? "active" : ""}
                  onClick={() => setSourceFilter(option.value)}
                >
                  {option.label}
                </button>
              ))}
            </div>
          </div>
        </div>
      ) : null}

      <div className="analyze-plot-host">
        {seriesRows.rows.length > 0 && filteredRunIds.size === 0 ? (
          <p className="empty-copy">{t("results.noFilterMatches")}</p>
        ) : seriesRows.rows.length > 0 ? (
          <div className="plot-skeleton" aria-label={t("analyze.plotTitle")}>
            {runSeries.length > 1 ? (
              <div className="plot-legend">
                {runSeries.map((series) => (
                  <label key={series.runId} className="plot-legend-item">
                    <input
                      type="checkbox"
                      checked={!hiddenRunIds.has(series.runId)}
                      onChange={() => toggleRunVisibility(series.runId)}
                    />
                    <span className="swatch" style={{ background: series.color }} />
                    <span>{series.label}</span>
                  </label>
                ))}
              </div>
            ) : null}
            <ErrorLinePlot
              data={plotData}
              logScale={logDisplay}
              threshold={PERFECTLY_DESCALE_THRESHOLD}
              title={plotTitle}
              xAxisLabel={t(axisMode === "w_only" ? "analyze.axisWidth" : "analyze.axisHeight")}
              yAxisLabel={t("analyze.axisRelativeError")}
              thresholdLabel={t("analyze.perfectThreshold")}
              bestKey={plotAids?.bestKey}
              selectedX={selectedHeight}
              valleyKeys={plotAids?.valleyKeys}
              onSelect={selectHeight}
              resetLabel={t("plot.resetZoom")}
              onZoomRangeChange={setZoomRange}
            />
            {plotAids ? (
              <>
                <button
                  type="button"
                  className={`plot-verdict ${plotAids.bestMetric <= PERFECTLY_DESCALE_THRESHOLD ? "perfect" : ""}`}
                  onClick={() => selectHeight(plotAids.bestHeight)}
                  title={t("analyze.verdictSelect")}
                >
                  <span className="plot-verdict-cell plot-verdict-hero">
                    <span className="plot-verdict-label">{t("analyze.verdict.height")}</span>
                    <strong>{plotAids.bestHeight}</strong>
                  </span>
                  <span className="plot-verdict-cell">
                    <span className="plot-verdict-label">{t("analyze.verdict.kernel")}</span>
                    <span className="plot-verdict-value">{plotAids.bestKernel}</span>
                  </span>
                  <span className="plot-verdict-cell">
                    <span className="plot-verdict-label">{t("analyze.verdict.error")}</span>
                    <span className="plot-verdict-value">
                      {plotAids.bestMetric.toExponential(2)}
                    </span>
                  </span>
                  <span className="plot-verdict-cell">
                    <span className="plot-verdict-label">≤1e-6</span>
                    <span className="plot-verdict-value">
                      {t("analyze.verdict.perfectCount", {
                        count: String(plotAids.perfectCount),
                        total: String(plotAids.totalCount),
                      })}
                    </span>
                  </span>
                </button>
                {plotAids.isEdgeFallback ? (
                  <span className="help-copy warning-copy">
                    {t("analyze.verdict.edgeOnly")}
                  </span>
                ) : null}
                {plotAids.candidates.length > 1 ? (
                  <div className="candidate-chips plot-valley-candidates">
                    <span className="help-copy">{t("analyze.verdict.candidates")}</span>
                    {plotAids.candidates.map((candidate) => (
                      <button
                        type="button"
                        key={candidate.height}
                        className={`candidate-chip ${sameHeight(selectedHeight, formatHeight(candidate.height)) ? "selected" : ""}`}
                        onClick={() => selectHeight(formatHeight(candidate.height))}
                        title={t("analyze.verdict.consensus", {
                          count: String(candidate.runCount),
                        })}
                      >
                        {formatHeight(candidate.height)}
                        {candidate.runCount > 1 ? ` ×${candidate.runCount}` : ""}
                      </button>
                    ))}
                  </div>
                ) : null}
              </>
            ) : null}
          </div>
        ) : (
          <BlockedState
            title={
              analyzeAvailable ? t("analyze.noRealRunsTitle") : t("analyze.blockedTitle")
            }
            body={
              analyzeAvailable
                ? t("analyze.noRealRunsBody")
                : `${t("analyze.blockedBody")} ${t("analyze.geometryHint")}`
            }
            action={
              <button className="secondary-button" type="button" onClick={onOpenDiagnostics}>
                {t("nav.diagnostics")}
              </button>
            }
          />
        )}
      </div>

      <div className="analyze-table-host height-results-table">
        <button
          type="button"
          className="analyze-table-toggle"
          aria-expanded={!isTableCollapsed}
          onClick={() => setTableCollapsed(!isTableCollapsed)}
        >
          {isTableCollapsed ? (
            <ChevronRight size={14} />
          ) : (
            <ChevronDown size={14} />
          )}
          <span>{t("analyze.resultsTable")}</span>
          <span className="analyze-table-count">
            {tableRows.length > HEIGHT_TABLE_LIMIT
              ? `${HEIGHT_TABLE_LIMIT} / ${tableRows.length}`
              : tableRows.length}
          </span>
        </button>
        {!isTableCollapsed ? (
          <>
            {kernelGroups.length > 1 ? (
              <div className="kernel-group-chips">
                <button
                  type="button"
                  className={`candidate-chip ${kernelFilter === null ? "selected" : ""}`}
                  onClick={() => setKernelFilter(null)}
                >
                  {t("analyze.allKernelGroups")}
                </button>
                {kernelGroups.map((group) => (
                  <button
                    key={group.key}
                    type="button"
                    className={`candidate-chip ${kernelFilter === group.key ? "selected" : ""}`}
                    onClick={() => setKernelFilter(group.key)}
                  >
                    {group.label}
                  </button>
                ))}
              </div>
            ) : null}
            {tableRows.length ? (
              <ResultMetricTable
                t={t}
                ariaLabel={t("analyze.resultsTable")}
                columns={[
                  t(axisMode === "w_only" ? "analyze.col.width" : "analyze.col.height"),
                  t("analyze.col.metric"),
                  t("analyze.col.sample"),
                  t("analyze.col.kernel"),
                  t("analyze.col.run"),
                ]}
                metricColumnIndex={1}
                columnTemplate="72px 96px minmax(80px, 1fr) 88px 72px"
                rows={tableRows}
                defaultMetricSort="asc"
                limit={HEIGHT_TABLE_LIMIT}
              />
            ) : (
              <div>
                <h3>{t("analyze.candidatesTitle")}</h3>
                {grid.ok ? (
                  <div
                    className="candidate-preview"
                    role="table"
                    aria-label={t("analyze.candidatesTitle")}
                  >
                    <div className="candidate-preview-meta">
                      {t("analyze.candidateCount", {
                        count: String(grid.grid.candidates.length),
                      })}
                      {work.ok
                        ? ` · ${t("analyze.workEstimate", { count: String(work.estimate) })}`
                        : ""}
                    </div>
                    <div className="candidate-chips">
                      {grid.grid.candidates.slice(0, 48).map((value) => (
                        <button
                          type="button"
                          key={value}
                          className={`candidate-chip ${selectedHeight === value ? "selected" : ""}`}
                          onClick={() => selectHeight(value)}
                        >
                          {value}
                        </button>
                      ))}
                      {grid.grid.candidates.length > 48 ? (
                        <span className="candidate-chip muted">
                          +{grid.grid.candidates.length - 48}
                        </span>
                      ) : null}
                    </div>
                  </div>
                ) : (
                  <p className="help-copy">{t("analyze.invalidGrid")}</p>
                )}
              </div>
            )}
          </>
        ) : null}
      </div>
    </section>
  );
}
