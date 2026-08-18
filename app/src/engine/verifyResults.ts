import type { Translator } from "../i18n";
import type { ProjectState, Run } from "../project/types";
import type { VerifyFrameEntry } from "./executeVerify";
import type { ScanScope, VerifyCoverage } from "./protocol";

type RecordValue = Record<string, unknown>;

function record(value: unknown): RecordValue | null {
  return value && typeof value === "object" && !Array.isArray(value)
    ? value as RecordValue
    : null;
}

function nonNegativeInteger(value: unknown): number | null {
  return Number.isSafeInteger(value) && (value as number) >= 0 ? value as number : null;
}

/** Stored verify result shape: engine payload plus orchestrator-merged frames. */
export function storedVerifyFrames(run: Run): VerifyFrameEntry[] | null {
  const frames = record(run.result)?.frames;
  if (!Array.isArray(frames)) return null;
  const rows: VerifyFrameEntry[] = [];
  for (const item of frames) {
    const row = record(item);
    if (!row || typeof row.frameIndex !== "number") continue;
    rows.push({
      seq: typeof row.seq === "number" ? row.seq : row.frameIndex,
      frameIndex: row.frameIndex,
      pts: typeof row.pts === "number" ? row.pts : null,
      timestampSeconds: typeof row.timestampSeconds === "number"
        ? row.timestampSeconds
        : null,
      error: typeof row.error === "number" ? row.error : null,
    });
  }
  return rows.length ? rows : null;
}

export function storedVerifyCoverage(run: Run): VerifyCoverage | null {
  const value = record(record(run.result)?.coverage);
  if (!value) return null;
  const selection = value.selection;
  if (selection !== "all" && selection !== "decoded_i_picture" && selection !== "every_n") {
    return null;
  }
  const eligibleFrames = nonNegativeInteger(value.eligibleFrames);
  const selectedFrames = nonNegativeInteger(value.selectedFrames);
  const processedFrames = nonNegativeInteger(value.processedFrames);
  const failedFrames = nonNegativeInteger(value.failedFrames);
  if (
    eligibleFrames === null || selectedFrames === null ||
    processedFrames === null || failedFrames === null
  ) {
    return null;
  }
  return { selection, eligibleFrames, selectedFrames, processedFrames, failedFrames };
}

export type VerifyRunScope = {
  selection: ScanScope["selection"];
  everyN: number | null;
  startFrame: number | null;
  endFrame: number | null;
};

export function verifyRunScope(run: Run): VerifyRunScope {
  const snapshot = record(run.inputSnapshot);
  const request = record(snapshot?.request);
  const scope = record(snapshot?.scanScope) ?? record(request?.scanScope);
  const coverage = storedVerifyCoverage(run);
  const selection = scope?.selection ?? coverage?.selection;
  return {
    selection: selection === "all" || selection === "every_n" || selection === "decoded_i_picture"
      ? selection
      : "all",
    everyN: nonNegativeInteger(scope?.everyN),
    startFrame: nonNegativeInteger(scope?.startFrame),
    endFrame: nonNegativeInteger(scope?.endFrame),
  };
}

export function verifyScopeLabel(t: Translator, scope: VerifyRunScope): string {
  let selection: string;
  if (scope.selection === "decoded_i_picture") selection = t("verify.ruleIPicture");
  else if (scope.selection === "every_n") {
    selection = scope.everyN && scope.everyN > 0
      ? t("verify.ruleEveryNValue", { n: String(scope.everyN) })
      : t("verify.ruleEveryN");
  } else selection = t("verify.scopeFull");

  if (scope.startFrame === null && scope.endFrame === null) return selection;
  return t("verify.scopeWithRange", {
    scope: selection,
    start: `#${scope.startFrame ?? 0}`,
    end: scope.endFrame === null ? t("verify.rangeEnd") : `#${scope.endFrame}`,
  });
}

export function verificationRunLabel(
  run: Run,
  state: Pick<ProjectState, "sourcesById" | "recipesById">,
  t: Translator,
): string {
  const source = run.sourceId ? state.sourcesById[run.sourceId] : null;
  const sourceLabel = source?.label || source?.path || run.sourceId || run.id;
  const snapshot = record(run.inputSnapshot);
  const request = record(snapshot?.request);
  const recipeId = typeof request?.recipeId === "string"
    ? request.recipeId
    : typeof snapshot?.recipeId === "string"
      ? snapshot.recipeId
      : null;
  const recipeLabel = recipeId
    ? state.recipesById[recipeId]?.name ?? recipeId
    : t("verify.unknownRecipe");
  return `${sourceLabel} · ${verifyScopeLabel(t, verifyRunScope(run))} · ${recipeLabel}`;
}

export type VerifyCoverageDisplay = {
  text: string;
  badge: "incomplete" | "partial" | null;
  metricsIncomplete: boolean;
};

export function verifyCoverageDisplay(run: Run): VerifyCoverageDisplay {
  const coverage = storedVerifyCoverage(run);
  const scope = verifyRunScope(run);
  const processed = coverage?.processedFrames ?? Math.max(0, run.completed);
  const eligible = coverage?.eligibleFrames
    ?? (scope.selection === "all" && run.total > 0 ? run.total : null);
  const terminalWithMetrics = run.status === "completed"
    || run.status === "partial"
    || run.status === "cancelled";
  const storedCount = storedVerifyFrames(run)?.length ?? 0;
  return {
    text: `${processed}/${eligible ?? "?"}`,
    badge: run.status === "partial" || run.status === "cancelled"
      ? "partial"
      : run.status === "completed" && scope.selection !== "all"
        ? "incomplete"
        : null,
    metricsIncomplete: terminalWithMetrics && storedCount < run.completed,
  };
}

export function reviewVerifyFrames(
  frames: VerifyFrameEntry[],
  threshold: number | null,
  limit: number,
): VerifyFrameEntry[] {
  const valid = frames.filter((frame) =>
    frame.error !== null && (threshold === null || frame.error > threshold));
  return [...valid]
    .sort((left, right) => (right.error as number) - (left.error as number))
    .slice(0, Math.max(1, limit));
}

export function worstVerifyFrameInRange(
  frames: VerifyFrameEntry[],
  range: { xMin: number; xMax: number },
): VerifyFrameEntry | null {
  let worst: VerifyFrameEntry | null = null;
  for (const frame of frames) {
    if (frame.error === null) continue;
    if (frame.frameIndex < range.xMin || frame.frameIndex > range.xMax) continue;
    if (!worst || frame.error > (worst.error as number)) worst = frame;
  }
  return worst;
}
