import type { CandidateGridSpec, EndpointRule, SearchPreset } from "./protocol";
import type { GridSemantics } from "./types";

// Matches the worker protocol cap (app/src-tauri/src/worker.rs): the app must
// not reject grids the engine would happily run (e.g. 500-800 @ 0.01 = 30001).
const MAX_CANDIDATES = 100_000;

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

type FixedDecimal = { units: bigint; scale: number };

function parseFixed(value: string): FixedDecimal {
  const trimmed = value.trim();
  const negative = trimmed.startsWith("-");
  const unsigned = negative ? trimmed.slice(1) : trimmed;
  const [whole, fraction = ""] = unsigned.split(".");
  const units = BigInt(`${whole}${fraction}` || "0") * (negative ? -1n : 1n);
  return { units, scale: fraction.length };
}

function rescale(value: FixedDecimal, scale: number): bigint {
  return value.units * 10n ** BigInt(scale - value.scale);
}

function formatFixed(units: bigint, scale: number): string {
  const negative = units < 0n;
  let digits = (negative ? -units : units).toString();
  if (scale > 0) {
    digits = digits.padStart(scale + 1, "0");
    digits = `${digits.slice(0, -scale)}.${digits.slice(-scale)}`;
    while (digits.includes(".") && digits.endsWith("0")) digits = digits.slice(0, -1);
    if (digits.endsWith(".")) digits = digits.slice(0, -1);
  }
  return negative && units !== 0n ? `-${digits}` : digits;
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
  gridSemantics?: GridSemantics;
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

  const semantics = input.gridSemantics ?? "decimal_fixed_point";
  const fixedStart = parseFixed(input.start);
  const fixedStop = parseFixed(input.stop);
  const fixedStep = parseFixed(input.step);
  const scale = Math.max(fixedStart.scale, fixedStop.scale, fixedStep.scale);
  const startUnits = rescale(fixedStart, scale);
  const stopUnits = rescale(fixedStop, scale);
  const stepUnits = rescale(fixedStep, scale);
  const candidates: string[] = [];
  let repeatedValue = start.n;
  for (let i = 0; i <= MAX_CANDIDATES; i += 1) {
    const exactUnits = startUnits + BigInt(i) * stepUnits;
    const runtimeValue =
      semantics === "repeated_addition"
        ? repeatedValue
        : semantics === "index_multiplication"
          ? start.n + i * step.n
          : Number(exactUnits) / 10 ** scale;
    const within =
      semantics === "decimal_fixed_point"
        ? input.endpointRule === "inclusive"
          ? exactUnits <= stopUnits
          : exactUnits < stopUnits
        : input.endpointRule === "inclusive"
          ? runtimeValue <= stop.n
          : runtimeValue < stop.n;
    if (!within) break;
    if (candidates.length >= MAX_CANDIDATES) {
      return { ok: false, reason: "grid_too_large" };
    }
    candidates.push(formatFixed(exactUnits, scale));
    if (semantics === "repeated_addition") repeatedValue += step.n;
  }
  if (candidates.length === 0) return { ok: false, reason: "grid_empty" };
  return { ok: true, candidates };
}

export function buildCandidateGrid(input: {
  axis: CandidateGridSpec["axis"];
  start: string;
  stop: string;
  step: string;
  endpointRule: EndpointRule;
  gridSemantics?: GridSemantics;
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
      gridSemantics: input.gridSemantics,
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
  if (!Number.isInteger(input.start) || !Number.isInteger(input.stop)) {
    return { ok: false, reason: "integer_grid_requires_integer_bounds" };
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
