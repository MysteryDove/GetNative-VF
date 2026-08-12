import { useMemo, useState } from "react";
import { ChevronDown, ChevronRight } from "lucide-react";
import type { Translator } from "../i18n";
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

/**
 * Height-scan results pane (center): plot with series legend and verdict,
 * collapsible result table with kernel-group filter, candidate preview when
 * no runs have landed yet.
 */
export function HeightResultsPanel({
  t,
  analyzeAvailable,
  seriesRows,
  grid,
  work,
  plotTitle,
  hasIncludedSamples,
  applyBusy,
  applyNotice,
  submitNotice,
  onOpenDiagnostics,
  onOpenApplyDialog,
  onRefineAroundSelection,
}: {
  t: Translator;
  analyzeAvailable: boolean;
  seriesRows: SeriesTable;
  grid: ReturnType<typeof resolveHeightGrid>;
  work: ReturnType<typeof estimateHeightWork>;
  plotTitle: string;
  hasIncludedSamples: boolean;
  applyBusy: boolean;
  applyNotice: string;
  submitNotice: string | null;
  onOpenDiagnostics: () => void;
  onOpenApplyDialog: () => void;
  onRefineAroundSelection: (height: string) => void;
}) {
  const [selectedHeight, setSelectedHeight] = useState<string | null>(null);
  const [logDisplay, setLogDisplay] = useState(true);
  // null = auto: expanded while empty (candidate preview), collapsed once results land.
  const [tableCollapsed, setTableCollapsed] = useState<boolean | null>(null);
  const [hiddenRunIds, setHiddenRunIds] = useState<Set<string>>(new Set());
  const [kernelFilter, setKernelFilter] = useState<string | null>(null);

  function toggleRunVisibility(runId: string) {
    setHiddenRunIds((current) => toggleSetValue(current, runId));
  }

  // One plot series per run (sample × kernel variant), each with a stable color.
  const runSeries = useMemo(
    () =>
      seriesRows.seriesMeta.map((meta, index) => ({
        ...meta,
        color: plotSeriesColor(index),
        label: `${meta.sampleLabel} · ${kernelMetaLabel(t, meta)}`,
      })),
    [seriesRows, t],
  );
  const runColorById = useMemo(
    () => new Map(runSeries.map((series) => [series.runId, series.color])),
    [runSeries],
  );
  const runLabelById = useMemo(
    () => new Map(runSeries.map((series) => [series.runId, series.label])),
    [runSeries],
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
        .filter((point) => !hiddenRunIds.has(point.runId))
        .map((point) => ({
          key: `${point.runId}-${point.height}`,
          runId: point.runId,
          x: point.height,
          metric: point.metric,
          color: runColorById.get(point.runId) ?? DEFAULT_SERIES_COLOR,
          label: runLabelById.get(point.runId),
        })),
    [seriesRows, hiddenRunIds, runColorById, runLabelById],
  );

  // Reading aids for the getnative workflow: valley (best point) and the
  // perfectly-descale threshold (error <= 1e-6), over the visible series.
  const plotAids = useMemo(() => {
    if (!plotData.length) return null;
    let best = plotData[0];
    for (const point of plotData) if (point.metric < best.metric) best = point;
    const perfectCount = plotData.filter(
      (point) => point.metric <= PERFECTLY_DESCALE_THRESHOLD,
    ).length;
    return {
      bestKey: best.key,
      bestHeight: best.x,
      bestMetric: best.metric,
      bestKernel: runKernelById.get(best.runId) ?? "",
      perfectCount,
    };
  }, [plotData, runKernelById]);

  // Kernel group selector for the result table (only when runs differ by kernel).
  const kernelGroups = useMemo(() => {
    const seen = new Map<string, string>();
    for (const meta of seriesRows.seriesMeta) {
      if (!seen.has(meta.kernelKey)) seen.set(meta.kernelKey, kernelMetaLabel(t, meta));
    }
    return [...seen.entries()].map(([key, label]) => ({ key, label }));
  }, [seriesRows, t]);

  const filteredRows = useMemo(
    () =>
      kernelFilter
        ? seriesRows.rows.filter((row) => row.kernelKey === kernelFilter)
        : seriesRows.rows,
    [seriesRows, kernelFilter],
  );

  const tableRows = useMemo(
    () =>
      filteredRows.map((row) => ({
        key: `${row.runId}-${row.height}`,
        metric: row.metric,
        cells: [row.height, row.sampleLabel, row.kernelId, row.runId.slice(0, 10)],
        selected: selectedHeight === row.height,
        onSelect: () => setSelectedHeight(row.height),
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
          onClick={onOpenApplyDialog}
        >
          {t("analyze.applyToRecipe")}
        </button>
        {applyNotice || submitNotice ? (
          <span className="help-copy">{applyNotice || submitNotice}</span>
        ) : null}
      </div>
      {seriesRows.incompatibleCount > 0 ? (
        <p className="help-copy warning-copy">
          {t("analyze.incompatibleMetricHidden", {
            count: String(seriesRows.incompatibleCount),
          })}
        </p>
      ) : null}

      <div className="analyze-plot-host">
        {seriesRows.rows.length > 0 ? (
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
              xAxisLabel={t("analyze.axisHeight")}
              yAxisLabel={t("analyze.axisRelativeError")}
              thresholdLabel={t("analyze.perfectThreshold")}
              bestKey={plotAids?.bestKey}
              selectedX={selectedHeight}
              onSelect={setSelectedHeight}
              resetLabel={t("plot.resetZoom")}
            />
            {plotAids ? (
              <button
                type="button"
                className={`plot-verdict ${plotAids.bestMetric <= PERFECTLY_DESCALE_THRESHOLD ? "perfect" : ""}`}
                onClick={() => setSelectedHeight(plotAids.bestHeight)}
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
                      total: String(plotData.length),
                    })}
                  </span>
                </span>
              </button>
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

      <div className="analyze-table-host">
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
          <span className="analyze-table-count">{tableRows.length}</span>
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
                  t("analyze.col.height"),
                  t("analyze.col.metric"),
                  t("analyze.col.sample"),
                  t("analyze.col.kernel"),
                  t("analyze.col.run"),
                ]}
                metricColumnIndex={1}
                columnTemplate="72px 96px minmax(80px, 1fr) 88px 72px"
                rows={tableRows}
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
                          onClick={() => setSelectedHeight(value)}
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
