import type { CandidateGridSpec, EndpointRule, SearchPreset } from "./protocol";

const MAX_CANDIDATES = 10_000;

function parseDecimal(value: string): { ok: true; n: number } | { ok: false; reason: string } {
  const trimmed = value.trim();
  if (!/^-?\d+(\.\d+)?$/.test(trimmed)) {
    return { ok: false, reason: "grid_not_decimal" };
  }
  const n = Number(trimmed);
  if (!Number.isFinite(n)) {
    return { ok: false, reason: "grid_not_finite" };
  }
  return { ok: true, n };
}

/** Format without binary float drift for common steps (1, 0.1, 0.25, 0.5). */
export function formatExactDecimal(value: number, stepText: string): string {
  const step = stepText.includes(".") ? stepText.split(".")[1]?.length ?? 0 : 0;
  if (step === 0) return String(Math.round(value));
  const fixed = value.toFixed(step);
  return fixed.replace(/\.?0+$/, (match) => (match.startsWith(".") ? "" : match));
}

export function resolveCandidateSequence(input: {
  start: string;
  stop: string;
  step: string;
  endpointRule: EndpointRule;
}): { ok: true; candidates: string[] } | { ok: false; reason: string } {
  const start = parseDecimal(input.start);
  const stop = parseDecimal(input.stop);
  const step = parseDecimal(input.step);
  if (!start.ok) return start;
  if (!stop.ok) return stop;
  if (!step.ok) return step;
  if (step.n === 0) return { ok: false, reason: "grid_step_zero" };
  if (step.n < 0) return { ok: false, reason: "grid_step_negative" };
  if (stop.n < start.n) return { ok: false, reason: "grid_stop_before_start" };

  const candidates: string[] = [];
  let value = start.n;
  // Inclusive loop with step-count bound; endpoint rule applied at stop.
  for (let i = 0; i < MAX_CANDIDATES; i += 1) {
    const atOrPastStop = value > stop.n + step.n * 1e-12;
    if (atOrPastStop) break;
    const exactlyAtStop = Math.abs(value - stop.n) <= step.n * 1e-12;
    if (exactlyAtStop && input.endpointRule === "exclusive_stop") break;
    candidates.push(formatExactDecimal(value, input.step));
    if (exactlyAtStop) break;
    value += step.n;
  }
  if (candidates.length === 0) return { ok: false, reason: "grid_empty" };
  if (candidates.length >= MAX_CANDIDATES) return { ok: false, reason: "grid_too_large" };
  return { ok: true, candidates };
}

export function buildCandidateGrid(input: {
  axis: CandidateGridSpec["axis"];
  start: string;
  stop: string;
  step: string;
  endpointRule: EndpointRule;
  preset?: SearchPreset | null;
}): { ok: true; grid: CandidateGridSpec } | { ok: false; reason: string } {
  const resolved = resolveCandidateSequence(input);
  if (!resolved.ok) return resolved;
  return {
    ok: true,
    grid: {
      axis: input.axis,
      start: input.start.trim(),
      stop: input.stop.trim(),
      step: input.step.trim(),
      endpointRule: input.endpointRule,
      candidates: resolved.candidates,
      preset: input.preset ?? null,
    },
  };
}

export function integerCoarseGrid(input: {
  start: number;
  stop: number;
  step?: number;
}): { ok: true; grid: CandidateGridSpec } | { ok: false; reason: string } {
  const step = input.step ?? 1;
  if (!Number.isInteger(input.start) || !Number.isInteger(input.stop) || !Number.isInteger(step)) {
    return { ok: false, reason: "integer_grid_requires_integers" };
  }
  return buildCandidateGrid({
    axis: "height",
    start: String(input.start),
    stop: String(input.stop),
    step: String(step),
    endpointRule: "inclusive",
    preset: "integer_coarse",
  });
}

/** Fractional refine around a selected height: selected ± halfSpan with step. */
export function fractionalRefineGrid(input: {
  selected: string;
  halfSpan: string;
  step: string;
}): { ok: true; grid: CandidateGridSpec } | { ok: false; reason: string } {
  const selected = parseDecimal(input.selected);
  const halfSpan = parseDecimal(input.halfSpan);
  if (!selected.ok) return selected;
  if (!halfSpan.ok) return halfSpan;
  if (halfSpan.n < 0) return { ok: false, reason: "refine_span_negative" };
  const start = formatExactDecimal(selected.n - halfSpan.n, input.step);
  const stop = formatExactDecimal(selected.n + halfSpan.n, input.step);
  return buildCandidateGrid({
    axis: "height",
    start,
    stop,
    step: input.step,
    endpointRule: "inclusive",
    preset: "fractional_refine",
  });
}

export function workEstimate(input: {
  sampleCount: number;
  fixedKernelCount: number;
  candidateCount: number;
}): number {
  return Math.max(0, input.sampleCount) * Math.max(0, input.fixedKernelCount) * Math.max(0, input.candidateCount);
}
