import type { Translator } from "../i18n";
import type { ProjectState, Run } from "../project/types";
import { runFamily } from "../project/runHistory";
import type { KernelRef } from "./protocol";
import { heightRunConfig, kernelMetaLabel } from "./runGroupPlan";
import { kernelRunTransfer } from "./kernelRunGroup";
import { verificationRunLabel } from "./verifyResults";

/**
 * One-line description of what a Run command measured, so a run can be told
 * apart (and safely deleted) without opening it: kernel and scan settings for
 * a Resolution Test, candidate count and geometry for a Kernel Search, the
 * Recipe and scope for a Check.
 */
export function runSelectionSummary(
  t: Translator,
  state: ProjectState,
  runIds: readonly string[],
): string {
  const runs = runIds
    .map((id) => state.runsById[id])
    .filter((run): run is Run => Boolean(run));
  const first = runs[0];
  if (!first) return "";
  const family = runFamily(first);
  if (family === "height") {
    const group = first.runGroupId ? state.runGroupsById[first.runGroupId] : null;
    const config = heightRunConfig(first, group);
    if (!config.step) return "";
    const kernels = new Set(
      runs.map((run) => {
        const kernel = heightRunConfig(run).kernel;
        return kernel
          ? kernelMetaLabel(t, { kernelId: kernel.id, kernelParameters: kernel.parameters })
          : "—";
      }),
    );
    return [
      kernels.size === 1
        ? [...kernels][0]
        : t("results.runKernelCount", { count: String(kernels.size) }),
      t("results.runScanSummary", {
        start: config.start ?? "",
        stop: config.stop ?? "",
        step: config.step,
        base: t(
          `analyze.baseMode.${config.axisMode === "w_only" ? config.baseWidthMode : config.baseHeightMode}`,
        ),
      }),
      ...(config.transfer !== "none" ? [t(`analyze.transfer.${config.transfer}`)] : []),
    ].join(" · ");
  }
  if (family === "kernel") {
    const snapshot = first.inputSnapshot as
      | { kernels?: KernelRef[]; geometry?: { canvasWidth?: number; canvasHeight?: number } }
      | null;
    const parts = [
      t("results.runKernelCount", { count: String(snapshot?.kernels?.length ?? 0) }),
    ];
    const geometry = snapshot?.geometry;
    if (geometry?.canvasWidth && geometry.canvasHeight) {
      parts.push(`${geometry.canvasWidth}×${geometry.canvasHeight}`);
    }
    const transfer = kernelRunTransfer(first);
    if (transfer !== "none") parts.push(t(`analyze.transfer.${transfer}`));
    return parts.join(" · ");
  }
  if (family === "verify") {
    // A Check command can cover several Recipes/scopes; name the first and
    // count the rest rather than implying the group is only that one.
    const labels = [...new Set(runs.map((run) => verificationRunLabel(run, state, t)))];
    return labels.length > 1 ? `${labels[0]} +${labels.length - 1}` : labels[0];
  }
  return "";
}

export type RunSelectorOption = {
  /** RunGroup id, or `run:<id>` for an ungrouped Run. */
  value: string;
  /** Short stable name, e.g. "Run 2" (numbered oldest first). */
  name: string;
  summary: string;
  /** Name plus summary, for tooltips and confirmations. */
  label: string;
  runIds: string[];
  /** True while any member is queued or running; such runs cannot be deleted. */
  active: boolean;
};

/** Run-selector entries for the Runs behind `runIds`, one per Run command. */
export function runSelectorOptions(
  t: Translator,
  state: ProjectState,
  runIds: Iterable<string>,
): RunSelectorOption[] {
  const members = new Map<string, string[]>();
  for (const runId of runIds) {
    const run = state.runsById[runId];
    const value = run?.runGroupId ? run.runGroupId : `run:${runId}`;
    const list = members.get(value) ?? [];
    if (!list.includes(runId)) list.push(runId);
    members.set(value, list);
  }
  const createdAt = (value: string) =>
    (value.startsWith("run:")
      ? state.runsById[value.slice(4)]?.createdAt
      : state.runGroupsById[value]?.createdAt) ?? "";
  return [...members.keys()]
    .sort((a, b) => createdAt(a).localeCompare(createdAt(b)) || a.localeCompare(b))
    .map((value, index) => {
      const ids = members.get(value) ?? [];
      const name = t("results.runOption", { number: String(index + 1) });
      const summary = runSelectionSummary(t, state, ids);
      return {
        value,
        name,
        summary,
        label: summary ? `${name} · ${summary}` : name,
        runIds: ids,
        active: ids.some((id) => {
          const status = state.runsById[id]?.status ?? "";
          return status === "queued" || status === "running";
        }),
      };
    });
}
