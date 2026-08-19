import { describe, expect, it } from "vitest";
import {
  KERNEL_CATEGORY_STEP,
  KERNEL_MIN_CATEGORY_STEP,
  KERNEL_MIN_PLOT_WIDTH,
  KERNEL_PLOT_MARGIN,
  kernelPlotDensity,
} from "./kernelPlotDensity";

const MARGINS = KERNEL_PLOT_MARGIN.left + KERNEL_PLOT_MARGIN.right;

describe("kernelPlotDensity", () => {
  it("keeps the historical fixed layout before the host is measured", () => {
    const density = kernelPlotDensity(30, null);
    expect(density.width).toBe(Math.max(KERNEL_MIN_PLOT_WIDTH, MARGINS + 30 * KERNEL_CATEGORY_STEP));
    expect(density.labelEvery).toBe(1);
    expect(density.pointRadius).toBe(3);
  });

  it("keeps natural spacing and the 560px floor while categories fit", () => {
    const density = kernelPlotDensity(8, 800);
    expect(density.width).toBe(KERNEL_MIN_PLOT_WIDTH);
    expect(density.labelEvery).toBe(1);
    expect(density.pointRadius).toBe(3);
  });

  it("compresses spacing to fit many categories into the host without scrolling", () => {
    const density = kernelPlotDensity(120, 800);
    expect(density.width).toBeLessThanOrEqual(800);
    // Points stay dense, labels thin out: 120 categories → ~13 labels.
    expect(density.labelEvery).toBe(9);
    expect(120 / density.labelEvery).toBeGreaterThan(10);
    expect(density.pointRadius).toBeLessThan(3);
  });

  it("thins labels proportionally to the compression ratio", () => {
    // host 560, 40 categories → step 12 → label every ceil(52/12) = 5.
    const density = kernelPlotDensity(40, 560);
    expect(density.width).toBeLessThanOrEqual(560);
    expect(density.labelEvery).toBe(5);
  });

  it("overflows and scrolls instead of going below the densest spacing", () => {
    const density = kernelPlotDensity(120, 300);
    expect(density.width).toBe(MARGINS + 120 * KERNEL_MIN_CATEGORY_STEP);
    expect(density.width).toBeGreaterThan(300);
    expect(density.pointRadius).toBe(1.5);
  });

  it("fits narrow hosts instead of scrolling for few categories", () => {
    const density = kernelPlotDensity(3, 400);
    expect(density.width).toBe(400);
    expect(density.labelEvery).toBe(1);
  });

  it("never drops below one label or a 1.5px point", () => {
    const density = kernelPlotDensity(1, 2000);
    expect(density.labelEvery).toBe(1);
    expect(density.pointRadius).toBeGreaterThanOrEqual(1.5);
  });
});
