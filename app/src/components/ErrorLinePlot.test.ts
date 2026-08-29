import { describe, expect, it } from "vitest";
import { decimateMinMax, MAX_MARKERS, windowSlice } from "./ErrorLinePlot";

type Pt = { x: number; y: number };

function series(n: number, x0 = 500, step = 1, y: (x: number) => number = (x) => x % 7): Pt[] {
  return Array.from({ length: n }, (_, i) => {
    const x = x0 + i * step;
    return { x, y: y(x) };
  });
}

describe("windowSlice", () => {
  const points = series(11, 0, 1); // x = 0..10

  it("returns the whole series when the window covers it", () => {
    expect(windowSlice(points, (p) => p.x, -1, 11)).toEqual(points);
  });

  it("keeps one point outside each edge so segments span the boundary", () => {
    const slice = windowSlice(points, (p) => p.x, 3, 6);
    expect(slice.map((p) => p.x)).toEqual([2, 3, 4, 5, 6, 7]);
  });

  it("keeps both neighbors when the window falls between two samples", () => {
    const slice = windowSlice(points, (p) => p.x, 4.2, 4.8);
    expect(slice.map((p) => p.x)).toEqual([4, 5]);
  });

  it("keeps the nearest edge point when the window is outside the data", () => {
    expect(windowSlice(points, (p) => p.x, 20, 30).map((p) => p.x)).toEqual([10]);
    expect(windowSlice(points, (p) => p.x, -30, -20).map((p) => p.x)).toEqual([0]);
  });
});

describe("decimateMinMax", () => {
  it("returns the input unchanged within budget", () => {
    const points = series(100);
    expect(decimateMinMax(points, (p) => p.y, 64)).toEqual(points);
  });

  it("preserves spikes that every-Nth sampling would drop", () => {
    const points = series(1000, 0, 1, (x) => (x === 633 ? 1e3 : 1));
    const out = decimateMinMax(points, (p) => p.y, 64);
    expect(out.length).toBeLessThanOrEqual(128);
    expect(out.some((p) => p.x === 633)).toBe(true);
  });
});

describe("zoom-aware marker budget", () => {
  it("a dense scan strided when whole becomes fully marked once zoomed", () => {
    // The reported bug: a 0.25-step scan over 500..1000 has 2001 points, so
    // the global stride hid 6 of 7 markers and polyline peaks appeared to
    // float between sample dots. Scoped to a zoom window, the stride is 1.
    const points = series(2001, 500, 0.25);
    const globalStride = Math.max(1, Math.ceil(points.length / MAX_MARKERS));
    expect(globalStride).toBeGreaterThan(1);

    const zoomed = windowSlice(points, (p) => p.x, 616, 646);
    expect(zoomed.length).toBeGreaterThan(0);
    const zoomStride = Math.max(1, Math.ceil(zoomed.length / MAX_MARKERS));
    expect(zoomStride).toBe(1);
  });
});
