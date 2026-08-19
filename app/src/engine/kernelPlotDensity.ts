/**
 * Auto density adaptation for the categorical kernel metric plot: keep the
 * comfortable per-kernel spacing while it fits the host, otherwise compress
 * spacing (thinning tick labels — points are never dropped); below the
 * densest allowed spacing the plot keeps its width and scrolls instead.
 */

export const KERNEL_PLOT_MARGIN = { top: 30, right: 18, bottom: 76, left: 62 } as const;
/** Comfortable per-kernel spacing (px). */
export const KERNEL_CATEGORY_STEP = 56;
/** Densest spacing before the plot gives up fitting and scrolls. */
export const KERNEL_MIN_CATEGORY_STEP = 4;
/** Horizontal room one multi-line kernel tick label needs (px). */
export const KERNEL_LABEL_MIN_STEP = 52;
export const KERNEL_MIN_PLOT_WIDTH = 560;

export interface KernelPlotDensity {
  /** Total svg width in px. */
  width: number;
  /** Render every Nth category tick label (1 = all). */
  labelEvery: number;
  /** Data point radius; the selected point draws 1.5px larger. */
  pointRadius: number;
}

/**
 * `hostWidth` is the chart host's content-box width, or null before the first
 * measure (and while the panel is display:none) — the null branch reproduces
 * the historical fixed-spacing layout.
 */
export function kernelPlotDensity(count: number, hostWidth: number | null): KernelPlotDensity {
  const margins = KERNEL_PLOT_MARGIN.left + KERNEL_PLOT_MARGIN.right;
  if (hostWidth == null || hostWidth <= 0) {
    return {
      width: Math.max(KERNEL_MIN_PLOT_WIDTH, margins + count * KERNEL_CATEGORY_STEP),
      labelEvery: 1,
      pointRadius: 3,
    };
  }
  const step =
    count > 1
      ? Math.min(
          KERNEL_CATEGORY_STEP,
          Math.max(KERNEL_MIN_CATEGORY_STEP, (hostWidth - margins) / count),
        )
      : KERNEL_CATEGORY_STEP;
  const compressedWidth = margins + count * step;
  const width =
    step < KERNEL_CATEGORY_STEP
      ? step > KERNEL_MIN_CATEGORY_STEP
        ? Math.min(compressedWidth, hostWidth) // absorb float epsilon → no 1px scrollbar
        : compressedWidth // densest allowed; overflows and scrolls
      : Math.max(Math.min(KERNEL_MIN_PLOT_WIDTH, hostWidth), compressedWidth);
  return {
    width,
    labelEvery: Math.max(1, Math.ceil(KERNEL_LABEL_MIN_STEP / step)),
    pointRadius: Math.max(1.5, Math.min(3, step / 2 - 1)),
  };
}
