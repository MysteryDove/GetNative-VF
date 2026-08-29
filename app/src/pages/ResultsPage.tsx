import { useMemo, useState } from "react";
import { ChevronDown, ChevronRight, Download, Trash2 } from "lucide-react";
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
import { runStatusLabel, runTypeLabel } from "../project/labels";
import { toggleSetValue } from "../utils/collections";
import { actualBackendLabel } from "../engine/backendSelection";
import type { ActualBackend } from "../engine/protocol";
import { verificationRunLabel, verifyCoverageDisplay } from "../engine/verifyResults";
import { sourceFilterLabel } from "../project/sourceLabel";
import { Modal } from "../components/Modal";

export { sourceFilterLabel } from "../project/sourceLabel";

const ACTIVE_STATUSES = new Set(["queued", "running"]);

export function runActualBackend(
  run: Run,
): { backend: ActualBackend; device?: string } | null {
  if (!run.result || typeof run.result !== "object") return null;
  const telemetry = (run.result as Record<string, unknown>).telemetry;
  if (!telemetry || typeof telemetry !== "object" || Array.isArray(telemetry)) return null;
  const values = telemetry as Record<string, unknown>;
  const backend = values.backend;
  if (
    backend !== "cpu"
    && backend !== "cuda"
    && backend !== "vulkan"
    && backend !== "metal"
  ) return null;
  const deviceKey = backend === "cuda"
    ? "cuda_device"
    : backend === "vulkan"
      ? "vulkan_device"
      : backend === "metal"
        ? "metal_device"
        : null;
  const device = deviceKey && typeof values[deviceKey] === "string"
    ? values[deviceKey] as string
    : undefined;
  return { backend, device };
}

export function runDecodeProvenance(
  run: Run,
): { decoder: string; zeroCopy: boolean; fallbackReason?: string } | null {
  if (!run.result || typeof run.result !== "object") return null;
  const provenance = (run.result as Record<string, unknown>).provenance;
  if (!provenance || typeof provenance !== "object" || Array.isArray(provenance)) {
    return null;
  }
  const values = provenance as Record<string, unknown>;
  if (typeof values.decoder !== "string" || typeof values.zero_copy !== "boolean") {
    return null;
  }
  let fallbackReason: string | undefined;
  if (Array.isArray(values.fallback_chain)) {
    for (const entry of values.fallback_chain) {
      if (entry && typeof entry === "object" && !Array.isArray(entry)) {
        const reason = (entry as Record<string, unknown>).reason;
        if (typeof reason === "string" && reason) fallbackReason = reason;
      }
    }
  }
  return { decoder: values.decoder, zeroCopy: values.zero_copy, fallbackReason };
}

function runGroupActive(state: ProjectState, group: RunGroup): boolean {
  return group.memberRunIds.some((id) => ACTIVE_STATUSES.has(state.runsById[id]?.status ?? ""));
}

export function removeRunGroupFromState(state: ProjectState, groupId: string): ProjectState {
  const group = state.runGroupsById[groupId];
  if (!group || runGroupActive(state, group)) return state;

  const runsById = { ...state.runsById };
  const verificationReviewsByRunId = { ...state.verificationReviewsByRunId };
  for (const id of group.memberRunIds) {
    delete runsById[id];
    delete verificationReviewsByRunId[id];
  }
  const runGroupsById = { ...state.runGroupsById };
  delete runGroupsById[group.id];
  return { ...state, runsById, runGroupsById, verificationReviewsByRunId };
}

export function nextHistoryFilters(
  filters: { runGroupFilter: string; sourceFilter: string },
  nextState: ProjectState,
  deleted: { kind: "group"; id: string } | { kind: "run"; run: Run },
): { runGroupFilter: string; sourceFilter: string } {
  let runGroupFilter = filters.runGroupFilter;
  let sourceFilter = filters.sourceFilter;
  if (deleted.kind === "group") {
    if (runGroupFilter === deleted.id) runGroupFilter = "all";
  } else {
    if (runGroupFilter === `run:${deleted.run.id}`) runGroupFilter = "all";
    if (
      deleted.run.runGroupId
      && runGroupFilter === deleted.run.runGroupId
      && nextState.runGroupsById[deleted.run.runGroupId] === undefined
    ) {
      runGroupFilter = "all";
    }
  }
  if (sourceFilter !== "all") {
    const sourceStillPresent = Object.values(nextState.runsById).some(
      (run) => run.sourceId === sourceFilter,
    );
    if (!sourceStillPresent) sourceFilter = "all";
  }
  return { runGroupFilter, sourceFilter };
}

export function removeRunFromState(state: ProjectState, runId: string): ProjectState {
  const run = state.runsById[runId];
  if (!run || ACTIVE_STATUSES.has(run.status)) return state;

  const runsById = { ...state.runsById };
  delete runsById[run.id];
  const verificationReviewsByRunId = { ...state.verificationReviewsByRunId };
  delete verificationReviewsByRunId[run.id];
  let runGroupsById = state.runGroupsById;
  const group = run.runGroupId ? state.runGroupsById[run.runGroupId] : null;
  if (group) {
    const memberRunIds = group.memberRunIds.filter((id) => id !== run.id);
    runGroupsById = { ...state.runGroupsById };
    if (memberRunIds.length === 0) delete runGroupsById[group.id];
    else runGroupsById[group.id] = { ...group, memberRunIds };
  }
  return { ...state, runsById, runGroupsById, verificationReviewsByRunId };
}

/**
 * Results: immutable Run/RunGroup history with provenance, compatibility-gated
 * comparison, and structured export. Historical inputs are never edited here.
 */
export function ResultsPage({
  t,
  state,
  onProjectChange,
}: {
  t: Translator;
  state: ProjectState;
  onProjectChange: (updater: (state: ProjectState) => ProjectState) => void;
}) {
  const [expandedGroups, setExpandedGroups] = useState<Set<string>>(new Set());
  const [selectedRuns, setSelectedRuns] = useState<Set<string>>(new Set());
  const [runGroupFilter, setRunGroupFilter] = useState("all");
  const [sourceFilter, setSourceFilter] = useState("all");
  const [notice, setNotice] = useState("");
  const [pendingDelete, setPendingDelete] = useState<
    { kind: "group"; id: string } | { kind: "run"; id: string } | null
  >(null);

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

  const runFilterOptions = useMemo(() => {
    const entries = [
      ...groups.map((group) => ({ value: group.id, createdAt: group.createdAt })),
      ...ungroupedRuns.map((run) => ({ value: `run:${run.id}`, createdAt: run.createdAt })),
    ].sort((a, b) => a.createdAt.localeCompare(b.createdAt) || a.value.localeCompare(b.value));
    return entries.map((entry, index) => ({
      value: entry.value,
      label: t("results.runOption", { number: String(index + 1) }),
    }));
  }, [groups, ungroupedRuns, t]);

  const sourceFilterOptions = useMemo(() => {
    const sourceIds = new Set<string>();
    for (const group of groups) {
      for (const runId of group.memberRunIds) {
        const sourceId = state.runsById[runId]?.sourceId;
        if (sourceId) sourceIds.add(sourceId);
      }
    }
    for (const run of ungroupedRuns) {
      if (run.sourceId) sourceIds.add(run.sourceId);
    }
    return [...sourceIds].map((sourceId) => ({
      value: sourceId,
      label: sourceFilterLabel(sourceId, state),
    }));
  }, [groups, state, ungroupedRuns]);

  const visibleGroups = useMemo(
    () =>
      groups.filter((group) => {
        if (runGroupFilter !== "all" && runGroupFilter !== group.id) return false;
        return sourceFilter === "all" || group.memberRunIds.some(
          (runId) => state.runsById[runId]?.sourceId === sourceFilter,
        );
      }),
    [groups, runGroupFilter, sourceFilter, state.runsById],
  );

  const visibleUngroupedRuns = useMemo(
    () =>
      ungroupedRuns.filter((run) => {
        if (runGroupFilter !== "all" && runGroupFilter !== `run:${run.id}`) return false;
        return sourceFilter === "all" || run.sourceId === sourceFilter;
      }),
    [runGroupFilter, sourceFilter, ungroupedRuns],
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
        label: runLabel(run, state, t),
        points: data.length,
        bestKey: best.seriesKey,
        bestMetric: best.metric,
      });
    }
    return rows;
  }, [selectedRuns, state, state.runsById, t]);

  function toggleGroup(groupId: string) {
    setExpandedGroups((current) => toggleSetValue(current, groupId));
  }

  function toggleRun(runId: string) {
    setSelectedRuns((current) => toggleSetValue(current, runId));
  }

  function groupActive(group: RunGroup): boolean {
    return runGroupActive(state, group);
  }

  function deleteGroup(group: RunGroup) {
    if (groupActive(group)) return;
    setPendingDelete({ kind: "group", id: group.id });
  }

  function deleteRun(run: Run) {
    if (ACTIVE_STATUSES.has(run.status)) return;
    setPendingDelete({ kind: "run", id: run.id });
  }

  function confirmDelete() {
    if (!pendingDelete) return;
    if (pendingDelete.kind === "group") {
      const group = state.runGroupsById[pendingDelete.id];
      if (!group || runGroupActive(state, group)) {
        setPendingDelete(null);
        return;
      }
      onProjectChange((current) => removeRunGroupFromState(current, pendingDelete.id));
      setSelectedRuns((current) => {
        const next = new Set(current);
        for (const id of group.memberRunIds) next.delete(id);
        return next;
      });
      const nextFilters = nextHistoryFilters(
        { runGroupFilter, sourceFilter },
        removeRunGroupFromState(state, pendingDelete.id),
        { kind: "group", id: pendingDelete.id },
      );
      setRunGroupFilter(nextFilters.runGroupFilter);
      setSourceFilter(nextFilters.sourceFilter);
    } else {
      const run = state.runsById[pendingDelete.id];
      if (!run || ACTIVE_STATUSES.has(run.status)) {
        setPendingDelete(null);
        return;
      }
      onProjectChange((current) => removeRunFromState(current, pendingDelete.id));
      setSelectedRuns((current) => {
        const next = new Set(current);
        next.delete(pendingDelete.id);
        return next;
      });
      const nextFilters = nextHistoryFilters(
        { runGroupFilter, sourceFilter },
        removeRunFromState(state, pendingDelete.id),
        { kind: "run", run },
      );
      setRunGroupFilter(nextFilters.runGroupFilter);
      setSourceFilter(nextFilters.sourceFilter);
    }
    setPendingDelete(null);
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

      <div className="results-filters" aria-label={t("results.filters.title")}>
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
            {visibleGroups.length === 0 && visibleUngroupedRuns.length === 0 ? (
              <p className="empty-copy">{t("results.noFilterMatches")}</p>
            ) : (
              <div className="dense-table">
              {visibleGroups.map((group) => (
                <RunGroupBlock
                  key={group.id}
                  t={t}
                  group={group}
                  state={state}
                  expanded={expandedGroups.has(group.id)}
                  onToggle={() => toggleGroup(group.id)}
                  onDelete={() => deleteGroup(group)}
                  deleteDisabled={groupActive(group)}
                  selectedRuns={selectedRuns}
                  onToggleRun={toggleRun}
                  onDeleteRun={deleteRun}
                  runSelectable={runSelectable}
                />
              ))}
              {visibleUngroupedRuns.map((run) => (
                <RunRow
                  key={run.id}
                  t={t}
                  run={run}
                  state={state}
                  selected={selectedRuns.has(run.id)}
                  onToggle={() => toggleRun(run.id)}
                  onDelete={() => deleteRun(run)}
                  deleteDisabled={ACTIVE_STATUSES.has(run.status)}
                  selectable={runSelectable(run)}
                />
              ))}
              </div>
            )}
          </section>
        </>
      )}

      {pendingDelete ? (
        <Modal
          onClose={() => setPendingDelete(null)}
          title={t(pendingDelete.kind === "group" ? "results.deleteGroup" : "results.deleteRun")}
          closeLabel={t("common.close")}
          actions={
            <>
              <button
                className="secondary-button"
                type="button"
                autoFocus
                onClick={() => setPendingDelete(null)}
              >
                {t("common.cancel")}
              </button>
              <button className="secondary-button danger-button" type="button" onClick={confirmDelete}>
                <Trash2 size={14} />
                {t(pendingDelete.kind === "group" ? "results.deleteGroup" : "results.deleteRun")}
              </button>
            </>
          }
        >
          <p className="confirm-dialog-copy">
            {pendingDelete.kind === "group"
              ? t("results.deleteGroupConfirm", {
                  label: state.runGroupsById[pendingDelete.id]?.label
                    || state.runGroupsById[pendingDelete.id]?.groupType
                    || pendingDelete.id,
                })
              : t("results.deleteRunConfirm")}
          </p>
        </Modal>
      ) : null}
    </div>
  );
}

function RunGroupBlock({
  t,
  group,
  state,
  expanded,
  onToggle,
  onDelete,
  deleteDisabled,
  selectedRuns,
  onToggleRun,
  onDeleteRun,
  runSelectable,
}: {
  t: Translator;
  group: RunGroup;
  state: ProjectState;
  expanded: boolean;
  onToggle: () => void;
  onDelete: () => void;
  deleteDisabled: boolean;
  selectedRuns: Set<string>;
  onToggleRun: (runId: string) => void;
  onDeleteRun: (run: Run) => void;
  runSelectable: (run: Run) => { selectable: boolean; reason: "no_results" | "incompatible" | null };
}) {
  const members = group.memberRunIds
    .map((id) => state.runsById[id])
    .filter((run): run is Run => Boolean(run));
  return (
    <div className="run-group-block">
      <div className="dense-row run-group-header">
        <button className="run-group-toggle" type="button" onClick={onToggle}>
          <strong>
            {expanded ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
            {group.label || group.groupType}
          </strong>
          <span>
            {t("analyze.memberCount", { count: String(members.length) })}
            {group.createdAt ? ` · ${group.createdAt.slice(0, 10)}` : ""}
          </span>
        </button>
        <button
          className="icon-button danger"
          type="button"
          disabled={deleteDisabled}
          title={deleteDisabled ? t("results.deleteActiveHint") : t("results.deleteGroup")}
          onClick={onDelete}
        >
          <Trash2 size={14} />
        </button>
      </div>
      {expanded
        ? members.map((run) => (
            <RunRow
              key={run.id}
              t={t}
              run={run}
              state={state}
              selected={selectedRuns.has(run.id)}
              onToggle={() => onToggleRun(run.id)}
              onDelete={() => onDeleteRun(run)}
              deleteDisabled={ACTIVE_STATUSES.has(run.status)}
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
  onDelete,
  deleteDisabled,
  selectable,
  indented,
}: {
  t: Translator;
  run: Run;
  state: ProjectState;
  selected: boolean;
  onToggle: () => void;
  onDelete?: () => void;
  deleteDisabled?: boolean;
  selectable: { selectable: boolean; reason: "no_results" | "incompatible" | null };
  indented?: boolean;
}) {
  const rows = extractRunRows(run);
  const actualBackend = runActualBackend(run);
  const decode = runDecodeProvenance(run);
  const verification = run.runType === "verification" || run.runType === "verify";
  const coverage = verification ? verifyCoverageDisplay(run) : null;
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
        <strong>{runLabel(run, state, t)}</strong>
      </label>
      <span>
        {runTypeLabel(run.runType, t)}
        {" · "}
        {runStatusLabel(run.status, t)}
        {coverage
          ? ` · ${t("verify.col.coverage")} ${coverage.text}`
          : run.total > 0 ? ` · ${run.completed}/${run.total}` : ""}
        {coverage?.badge
          ? ` · ${t(coverage.badge === "partial"
              ? "verify.coveragePartial"
              : "verify.coverageIncomplete")}`
          : ""}
        {rows.length ? ` · ${t("results.points", { count: String(rows.length) })}` : ""}
        {actualBackend
          ? ` · ${t("results.actualBackend", {
              backend: actualBackendLabel(t, actualBackend.backend, actualBackend.device),
            })}`
          : ""}
        {decode
          ? ` · ${t("results.decoder", { decoder: decode.decoder })} · ${t("results.zeroCopy", {
              state: t(decode.zeroCopy ? "common.yes" : "common.no"),
            })}${decode.fallbackReason
              ? ` · ${t("results.decodeFallback", { reason: decode.fallbackReason })}`
              : ""}`
          : ""}
      </span>
      {coverage?.metricsIncomplete ? (
        <span className="verify-metrics-warning">{t("verify.metricsIncomplete")}</span>
      ) : null}
      {onDelete ? (
        <button
          className="icon-button danger"
          type="button"
          disabled={deleteDisabled}
          title={deleteDisabled ? t("results.deleteActiveHint") : t("results.deleteRun")}
          onClick={onDelete}
        >
          <Trash2 size={14} />
        </button>
      ) : null}
    </div>
  );
}

function runLabel(run: Run, state: ProjectState, t: Translator): string {
  if (run.runType === "verification" || run.runType === "verify") {
    return verificationRunLabel(run, state, t);
  }
  const sample = run.sampleId ? state.samplesById[run.sampleId] : null;
  if (sample) return sample.label || sample.id;
  const source = run.sourceId ? state.sourcesById[run.sourceId] : null;
  if (source) return source.label || source.path;
  return run.id.slice(0, 14);
}
