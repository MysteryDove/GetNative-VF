import type { ProjectState, Run } from "./types";

/**
 * Run history bookkeeping shared by the Results page and the per-test run
 * selectors: which test a Run belongs to, and removing Runs while keeping
 * RunGroups and reviews consistent. Queued/running Runs are never removed.
 */

export const ACTIVE_RUN_STATUSES = new Set(["queued", "running"]);

export type RunFamily = "height" | "kernel" | "verify";

export const RUN_FAMILIES: readonly RunFamily[] = ["height", "kernel", "verify"];

export function runFamily(run: Pick<Run, "runType">): RunFamily | null {
  if (run.runType === "height") return "height";
  if (run.runType === "kernel") return "kernel";
  if (run.runType === "verification" || run.runType === "verify") return "verify";
  return null;
}

export function runFamilyCounts(
  state: Pick<ProjectState, "runsById">,
): Record<RunFamily, { total: number; active: number }> {
  const counts: Record<RunFamily, { total: number; active: number }> = {
    height: { total: 0, active: 0 },
    kernel: { total: 0, active: 0 },
    verify: { total: 0, active: 0 },
  };
  for (const run of Object.values(state.runsById)) {
    const family = runFamily(run);
    if (!family) continue;
    counts[family].total += 1;
    if (ACTIVE_RUN_STATUSES.has(run.status)) counts[family].active += 1;
  }
  return counts;
}

/** Remove the given Runs (active ones are skipped) and prune emptied RunGroups. */
export function removeRunsFromState(state: ProjectState, runIds: Iterable<string>): ProjectState {
  const removable = new Set<string>();
  for (const id of runIds) {
    const run = state.runsById[id];
    if (run && !ACTIVE_RUN_STATUSES.has(run.status)) removable.add(id);
  }
  if (removable.size === 0) return state;
  const runsById = { ...state.runsById };
  const verificationReviewsByRunId = { ...state.verificationReviewsByRunId };
  for (const id of removable) {
    delete runsById[id];
    delete verificationReviewsByRunId[id];
  }
  const runGroupsById: ProjectState["runGroupsById"] = {};
  for (const group of Object.values(state.runGroupsById)) {
    const memberRunIds = group.memberRunIds.filter((id) => !removable.has(id));
    if (memberRunIds.length === group.memberRunIds.length) runGroupsById[group.id] = group;
    else if (memberRunIds.length > 0) runGroupsById[group.id] = { ...group, memberRunIds };
  }
  return { ...state, runsById, runGroupsById, verificationReviewsByRunId };
}

/**
 * Remove every finished Run of one test. Clearing Check also drops saved
 * Fusion results once no Check Run is left for them to refer to.
 */
export function removeRunFamilyFromState(state: ProjectState, family: RunFamily): ProjectState {
  const next = removeRunsFromState(
    state,
    Object.values(state.runsById)
      .filter((run) => runFamily(run) === family)
      .map((run) => run.id),
  );
  if (family !== "verify" || next === state) return next;
  const checkRunLeft = Object.values(next.runsById).some((run) => runFamily(run) === "verify");
  return checkRunLeft ? next : { ...next, verificationFusionsById: {} };
}

/** Run ids behind a run-selector value: a RunGroup id, or `run:<id>` when ungrouped. */
export function runSelectionRunIds(
  state: Pick<ProjectState, "runGroupsById">,
  value: string,
): string[] {
  if (value.startsWith("run:")) return [value.slice(4)];
  return state.runGroupsById[value]?.memberRunIds ?? [];
}

/** Selector value a Run is listed under: its RunGroup, or itself when ungrouped. */
export function runSelectionValue(run: Pick<Run, "id" | "runGroupId"> | undefined, runId: string): string {
  return run?.runGroupId ? run.runGroupId : `run:${runId}`;
}
