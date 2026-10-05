import { kernelResultKey, type KernelResultRow } from "./kernelRunGroup";

/** One (b, c) cell of the Bicubic grid, averaged over the visible Samples/Runs. */
export type BicubicCell = {
  b: number;
  c: number;
  /** Mean relative error across the rows that measured this (b, c). */
  metric: number;
  count: number;
  /** Result keys behind the cell; the first is the lowest-error row. */
  keys: string[];
};

export type BicubicGrid = {
  bs: number[];
  cs: number[];
  cells: Map<string, BicubicCell>;
  best: BicubicCell;
  min: number;
  max: number;
};

export const bicubicCellKey = (b: number, c: number) => `${b}|${c}`;

/** Rows that belong on the b × c grid: plain Bicubic, default blur. */
export function isBicubicGridRow(row: KernelResultRow): boolean {
  if (row.kernelId !== "bicubic") return false;
  const { b, c, blur } = row.parameters;
  return Number.isFinite(Number(b)) && Number.isFinite(Number(c))
    && (blur === undefined || Number(blur) === 1);
}

/** True when the row is drawn as a cell of `grid` (and so leaves the line plot). */
export function isRowInBicubicGrid(grid: BicubicGrid, row: KernelResultRow): boolean {
  return isBicubicGridRow(row)
    && grid.cells.has(bicubicCellKey(Number(row.parameters.b), Number(row.parameters.c)));
}

/**
 * Build the b × c grid, or null when the rows do not form one (fewer than two
 * distinct values on either axis, too few points, or a sparse scatter).
 */
export function buildBicubicGrid(rows: KernelResultRow[], minimumCells = 6): BicubicGrid | null {
  const groups = new Map<string, KernelResultRow[]>();
  for (const row of rows) {
    if (!isBicubicGridRow(row) || !Number.isFinite(row.metric)) continue;
    const key = bicubicCellKey(Number(row.parameters.b), Number(row.parameters.c));
    groups.set(key, [...(groups.get(key) ?? []), row]);
  }
  const cells = new Map<string, BicubicCell>();
  for (const [key, members] of groups) {
    const ordered = [...members].sort((a, b) => a.metric - b.metric);
    cells.set(key, {
      b: Number(ordered[0].parameters.b),
      c: Number(ordered[0].parameters.c),
      metric: ordered.reduce((sum, row) => sum + row.metric, 0) / ordered.length,
      count: ordered.length,
      keys: ordered.map(kernelResultKey),
    });
  }
  // Only a lattice is a grid: drop points whose b or c no other point shares
  // (the scattered presets of the default scan list), repeating until stable.
  for (let changed = true; changed;) {
    changed = false;
    const perB = new Map<number, number>();
    const perC = new Map<number, number>();
    for (const cell of cells.values()) {
      perB.set(cell.b, (perB.get(cell.b) ?? 0) + 1);
      perC.set(cell.c, (perC.get(cell.c) ?? 0) + 1);
    }
    for (const [key, cell] of cells) {
      if ((perB.get(cell.b) ?? 0) < 2 || (perC.get(cell.c) ?? 0) < 2) {
        cells.delete(key);
        changed = true;
      }
    }
  }
  const values = [...cells.values()];
  const bs = [...new Set(values.map((cell) => cell.b))].sort((a, b) => a - b);
  const cs = [...new Set(values.map((cell) => cell.c))].sort((a, b) => a - b);
  if (bs.length < 2 || cs.length < 2 || values.length < minimumCells) return null;
  // A mostly empty lattice reads worse than the line plot.
  if (values.length * 2 < bs.length * cs.length) return null;
  const best = values.reduce((a, b) => (b.metric < a.metric ? b : a));
  return {
    bs,
    cs,
    cells,
    best,
    min: best.metric,
    max: values.reduce((a, b) => Math.max(a, b.metric), 0),
  };
}

/** 0..steps-1 on a log scale between the grid's lowest and highest error. */
export function heatStep(metric: number, min: number, max: number, steps: number): number {
  const lo = Math.log10(Math.max(min, 1e-12));
  const hi = Math.log10(Math.max(max, 1e-12));
  if (!(hi > lo)) return 0;
  const t = (Math.log10(Math.max(metric, 1e-12)) - lo) / (hi - lo);
  return Math.min(steps - 1, Math.max(0, Math.floor(t * steps)));
}

export type KernelRanking = {
  label: string;
  mean: number;
  worst: number;
  count: number;
  /** Lowest-error underlying row, used when the ranking row is selected. */
  key: string;
  keys: string[];
};

/** Rank kernels across Samples/Runs by mean error; `worst` shows the outlier. */
export function rankKernelsAcrossSamples(
  rows: Array<{ key: string; metric: number; label: string }>,
): KernelRanking[] {
  const groups = new Map<string, Array<{ key: string; metric: number }>>();
  for (const row of rows) {
    if (!Number.isFinite(row.metric)) continue;
    groups.set(row.label, [...(groups.get(row.label) ?? []), row]);
  }
  return [...groups.entries()].map(([label, members]) => {
    const ordered = [...members].sort((a, b) => a.metric - b.metric);
    return {
      label,
      mean: ordered.reduce((sum, row) => sum + row.metric, 0) / ordered.length,
      worst: ordered[ordered.length - 1].metric,
      count: ordered.length,
      key: ordered[0].key,
      keys: ordered.map((row) => row.key),
    };
  }).sort((a, b) => a.mean - b.mean);
}
