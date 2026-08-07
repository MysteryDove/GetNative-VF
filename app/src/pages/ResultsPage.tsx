import { useMemo, useState } from "react";
import { ChevronDown, ChevronRight, Download } from "lucide-react";
import type { Translator } from "../i18n";
import { metricCompatibilityKey } from "../engine/runGroupPlan";
import {
  buildRunExportCsv,
  buildRunExportJson,
  extractRunRows,
  hasExportableRows,
  runMetricSpec,
  saveArtifact,
} from "../project/export";
import type { ProjectState, Run, RunGroup } from "../project/types";

/**
 * Results: immutable Run/RunGroup history with provenance, compatibility-gated
 * comparison, and structured export. Historical inputs are never edited here.
 */
export function ResultsPage({
  t,
  state,
}: {
  t: Translator;
  state: ProjectState;
}) {
  const [expandedGroups, setExpandedGroups] = useState<Set<string>>(new Set());
  const [selectedRuns, setSelectedRuns] = useState<Set<string>>(new Set());
  const [notice, setNotice] = useState("");

  const groups = useMemo(
    () =>
      Object.values(state.runGroupsById).sort((a, b) =>
        b.createdAt.localeCompare(a.createdAt),
      ),
    [state.runGroupsById],
  );
  const ungroupedRuns = useMemo(
    () =>
      Object.values(state.runsById)
        .filter((run) => !run.runGroupId || !state.runGroupsById[run.runGroupId])
        .sort((a, b) => b.createdAt.localeCompare(a.createdAt)),
    [state.runsById, state.runGroupsById],
  );

  /** MetricSpec key of the current selection; incompatible Runs cannot join. */
  const selectionMetricKey = useMemo(() => {
    for (const id of selectedRuns) {
      const run = state.runsById[id];
      const metric = run ? runMetricSpec(run) : null;
      if (metric) return metricCompatibilityKey(metric);
    }
    return null;
  }, [selectedRuns, state.runsById]);

  const comparison = useMemo(() => {
    const rows: Array<{
      runId: string;
      label: string;
      points: number;
      bestKey: string;
      bestMetric: number;
    }> = [];
    for (const id of selectedRuns) {
      const run = state.runsById[id];
      if (!run) continue;
      const data = extractRunRows(run);
      if (!data.length) continue;
      const best = data.reduce((a, b) => (b.metric < a.metric ? b : a));
      rows.push({
        runId: run.id,
        label: runLabel(run, state),
        points: data.length,
        bestKey: best.seriesKey,
        bestMetric: best.metric,
      });
    }
    return rows;
  }, [selectedRuns, state, state.runsById]);

  function toggleGroup(groupId: string) {
    setExpandedGroups((current) => {
      const next = new Set(current);
      if (next.has(groupId)) next.delete(groupId);
      else next.add(groupId);
      return next;
    });
  }

  function toggleRun(runId: string) {
    setSelectedRuns((current) => {
      const next = new Set(current);
      if (next.has(runId)) next.delete(runId);
      else next.add(runId);
      return next;
    });
  }

  function runSelectable(run: Run): { selectable: boolean; reason: "no_results" | "incompatible" | null } {
    if (!extractRunRows(run).length) return { selectable: false, reason: "no_results" };
    if (selectionMetricKey && !selectedRuns.has(run.id)) {
      const metric = runMetricSpec(run);
      if (metric && metricCompatibilityKey(metric) !== selectionMetricKey) {
        return { selectable: false, reason: "incompatible" };
      }
    }
    return { selectable: true, reason: null };
  }

  async function exportSelected(format: "json" | "csv") {
    const ids = [...selectedRuns];
    if (!ids.length) return;
    setNotice("");
    try {
      if (format === "json") {
        const content = buildRunExportJson(state, ids);
        const path = await saveArtifact({
          defaultName: `${state.project.name || "runs"}-export`,
          extension: "json",
          content,
        });
        setNotice(t("results.exported", { path }));
      } else {
        const content = buildRunExportCsv(ids, state);
        if (!content) {
          setNotice(t("results.noExportableData"));
          return;
        }
        const path = await saveArtifact({
          defaultName: `${state.project.name || "runs"}-metrics`,
          extension: "csv",
          content,
        });
        setNotice(t("results.exported", { path }));
      }
    } catch (error) {
      if (String(error) !== "cancelled") setNotice(String(error));
    }
  }

  const isEmpty = groups.length === 0 && ungroupedRuns.length === 0;

  return (
    <div className="page-panel">
      <div className="page-header">
        <h2>{t("results.title")}</h2>
        <div className="top-actions">
          <button
            className="secondary-button"
            type="button"
            disabled={selectedRuns.size === 0}
            onClick={() => exportSelected("json")}
          >
            <Download size={15} />
            {t("results.exportJson")}
          </button>
          <button
            className="secondary-button"
            type="button"
            disabled={selectedRuns.size === 0 || !hasExportableRows(state, [...selectedRuns])}
            onClick={() => exportSelected("csv")}
          >
            <Download size={15} />
            {t("results.exportCsv")}
          </button>
        </div>
      </div>

      {notice ? <p className="help-copy">{notice}</p> : null}

      {isEmpty ? (
        <section className="page-section">
          <p className="empty-copy">{t("results.emptyBody")}</p>
        </section>
      ) : (
        <>
          {comparison.length > 0 ? (
            <section className="page-section">
              <h3>{t("results.comparison")}</h3>
              <div className="result-table" role="table" aria-label={t("results.comparison")}>
                <div className="result-table-head" role="row">
                  <span role="columnheader">{t("results.col.run")}</span>
                  <span role="columnheader">{t("results.col.points")}</span>
                  <span role="columnheader">{t("results.col.best")}</span>
                  <span role="columnheader">{t("results.col.bestMetric")}</span>
                </div>
                {comparison.map((row) => (
                  <div className="result-table-row" role="row" key={row.runId}>
                    <span role="cell">{row.label}</span>
                    <span role="cell">{row.points}</span>
                    <span role="cell">{row.bestKey}</span>
                    <span role="cell">{row.bestMetric.toPrecision(6)}</span>
                  </div>
                ))}
              </div>
            </section>
          ) : null}

          <section className="page-section">
            <h3>{t("results.groups")}</h3>
            <div className="dense-table">
              {groups.map((group) => (
                <RunGroupBlock
                  key={group.id}
                  t={t}
                  group={group}
                  state={state}
                  expanded={expandedGroups.has(group.id)}
                  onToggle={() => toggleGroup(group.id)}
                  selectedRuns={selectedRuns}
                  onToggleRun={toggleRun}
                  runSelectable={runSelectable}
                />
              ))}
              {ungroupedRuns.map((run) => (
                <RunRow
                  key={run.id}
                  t={t}
                  run={run}
                  state={state}
                  selected={selectedRuns.has(run.id)}
                  onToggle={() => toggleRun(run.id)}
                  selectable={runSelectable(run)}
                />
              ))}
            </div>
          </section>
        </>
      )}
    </div>
  );
}

function RunGroupBlock({
  t,
  group,
  state,
  expanded,
  onToggle,
  selectedRuns,
  onToggleRun,
  runSelectable,
}: {
  t: Translator;
  group: RunGroup;
  state: ProjectState;
  expanded: boolean;
  onToggle: () => void;
  selectedRuns: Set<string>;
  onToggleRun: (runId: string) => void;
  runSelectable: (run: Run) => { selectable: boolean; reason: "no_results" | "incompatible" | null };
}) {
  const members = group.memberRunIds
    .map((id) => state.runsById[id])
    .filter((run): run is Run => Boolean(run));
  return (
    <div className="run-group-block">
      <button className="dense-row run-group-header" type="button" onClick={onToggle}>
        <strong>
          {expanded ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
          {group.label || group.groupType}
        </strong>
        <span>
          {t("analyze.memberCount", { count: String(members.length) })}
          {group.createdAt ? ` · ${group.createdAt.slice(0, 10)}` : ""}
        </span>
      </button>
      {expanded
        ? members.map((run) => (
            <RunRow
              key={run.id}
              t={t}
              run={run}
              state={state}
              selected={selectedRuns.has(run.id)}
              onToggle={() => onToggleRun(run.id)}
              selectable={runSelectable(run)}
              indented
            />
          ))
        : null}
    </div>
  );
}

function RunRow({
  t,
  run,
  state,
  selected,
  onToggle,
  selectable,
  indented,
}: {
  t: Translator;
  run: Run;
  state: ProjectState;
  selected: boolean;
  onToggle: () => void;
  selectable: { selectable: boolean; reason: "no_results" | "incompatible" | null };
  indented?: boolean;
}) {
  const rows = extractRunRows(run);
  return (
    <div className={`dense-row run-row ${indented ? "indented" : ""}`}>
      <label className="checkbox-row">
        <input
          type="checkbox"
          checked={selected}
          disabled={!selectable.selectable}
          title={
            selectable.reason === "incompatible"
              ? t("results.incompatible")
              : selectable.reason === "no_results"
                ? t("results.noResults")
                : undefined
          }
          onChange={onToggle}
        />
        <strong>{runLabel(run, state)}</strong>
      </label>
      <span>
        {runTypeLabel(run.runType, t)}
        {" · "}
        {runStatusLabel(run.status, t)}
        {run.total > 0 ? ` · ${run.completed}/${run.total}` : ""}
        {rows.length ? ` · ${t("results.points", { count: String(rows.length) })}` : ""}
      </span>
    </div>
  );
}

function runLabel(run: Run, state: ProjectState): string {
  const sample = run.sampleId ? state.samplesById[run.sampleId] : null;
  if (sample) return sample.label || sample.id;
  const source = run.sourceId ? state.sourcesById[run.sourceId] : null;
  if (source) return source.label || source.path;
  return run.id.slice(0, 14);
}

function runTypeLabel(value: string, t: Translator): string {
  if (value === "height" || value === "height_analysis") return t("overview.run.resolution");
  if (value === "kernel" || value === "kernel_analysis") return t("overview.run.algorithm");
  if (value === "verification" || value === "verify") return t("overview.run.check");
  return t("overview.run.unknown");
}

function runStatusLabel(value: string, t: Translator): string {
  if (value === "queued") return t("overview.runStatus.queued");
  if (value === "running") return t("overview.runStatus.running");
  if (value === "completed") return t("overview.runStatus.completed");
  if (value === "failed") return t("overview.runStatus.failed");
  if (value === "cancelled") return t("overview.runStatus.cancelled");
  if (value === "partial") return t("overview.runStatus.partial");
  return t("overview.runStatus.unknown");
}
