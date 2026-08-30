import { describe, expect, it } from "vitest";
import {
  clusterValleys,
  detectValleys,
  findSeriesValleys,
  type ValleySeriesPoint,
} from "./valleyDetect";

/** Deterministic tiny PRNG (mulberry32) so noise curves are stable in CI. */
function mulberry32(seed: number) {
  let state = seed;
  return () => {
    state |= 0;
    state = (state + 0x6d2b79f5) | 0;
    let t = Math.imul(state ^ (state >>> 15), 1 | state);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

type Dip = { x: number; depth: number; width: number };

/**
 * Synthetic scan curve in the log10 domain: a monotonic downtrend (like a
 * resolution sweep where error keeps dropping) plus gaussian dips at the
 * given heights, plus point noise. Returned as metric = 10^y.
 */
function syntheticSeries(
  dips: Dip[],
  { n = 501, x0 = 500, step = 1, trend = -0.004, noise = 0, seed = 42 } = {},
): ValleySeriesPoint[] {
  const rand = mulberry32(seed);
  const points: ValleySeriesPoint[] = [];
  for (let i = 0; i < n; i += 1) {
    const x = x0 + i * step;
    let y = trend * (x - x0);
    for (const dip of dips) {
      const z = (x - dip.x) / dip.width;
      y -= dip.depth * Math.exp(-z * z);
    }
    if (noise > 0) y += (rand() - 0.5) * 2 * noise;
    points.push({ x, metric: 10 ** y, key: `run-${x}` });
  }
  return points;
}

describe("findSeriesValleys", () => {
  it("finds the true dip under a monotonic trend, not the edge", () => {
    const points = syntheticSeries([{ x: 870, depth: 0.5, width: 3 }], { noise: 0.02 });
    const valleys = findSeriesValleys({ runId: "run", points });
    expect(valleys.length).toBeGreaterThan(0);
    expect(valleys[0].x).toBe(870);
    expect(valleys[0].prominence).toBeGreaterThan(0.3);
  });

  it("reports nothing for a pure trend", () => {
    const points = syntheticSeries([], { noise: 0 });
    expect(findSeriesValleys({ runId: "run", points })).toEqual([]);
  });

  it("keeps point noise below the prominence threshold", () => {
    const points = syntheticSeries([], { noise: 0.02, seed: 7 });
    expect(findSeriesValleys({ runId: "run", points })).toEqual([]);
  });

  it("rejects dips shallower than minProminence", () => {
    const points = syntheticSeries([{ x: 870, depth: 0.05, width: 3 }], { trend: -0.001 });
    expect(findSeriesValleys({ runId: "run", points })).toEqual([]);
    const lenient = findSeriesValleys(
      { runId: "run", points },
      { minProminence: 0.02 },
    );
    expect(lenient.map((v) => v.x)).toContain(870);
  });

  it("handles tiny series without crashing", () => {
    const points: ValleySeriesPoint[] = [
      { x: 1, metric: 1 },
      { x: 2, metric: 0.5 },
    ];
    expect(findSeriesValleys({ runId: "run", points })).toEqual([]);
  });
});

describe("clusterValleys", () => {
  it("merges heights within tolerance into one consensus cluster", () => {
    const merged = clusterValleys([
      { runId: "a", x: 870, metric: 1e-4, prominence: 0.3 },
      { runId: "b", x: 870.4, metric: 2e-4, prominence: 0.3 },
    ]);
    expect(merged).toHaveLength(1);
    expect(merged[0].runCount).toBe(2);
    expect(merged[0].height).toBe(870); // deepest member's measured height

    const split = clusterValleys(
      [
        { runId: "a", x: 870, metric: 1e-4, prominence: 0.3 },
        { runId: "b", x: 870.4, metric: 2e-4, prominence: 0.3 },
      ],
      { clusterTolerance: 0.2 },
    );
    expect(split).toHaveLength(2);
  });
});

describe("detectValleys", () => {
  it("ranks the corroborated height first across runs", () => {
    const a = syntheticSeries([{ x: 870, depth: 0.3, width: 3 }], { noise: 0.01, seed: 1 });
    const b = syntheticSeries(
      [
        { x: 870, depth: 0.3, width: 3 },
        { x: 620, depth: 0.2, width: 3 },
      ],
      { noise: 0.01, seed: 2 },
    );
    const { candidates } = detectValleys([
      { runId: "a", points: a },
      { runId: "b", points: b },
    ]);
    expect(candidates.length).toBeGreaterThanOrEqual(2);
    expect(candidates[0].height).toBeCloseTo(870, 0);
    expect(candidates[0].runCount).toBe(2);
  });

  it("falls back to the global minimum when no valley stands out", () => {
    const points = syntheticSeries([], { noise: 0 });
    const { candidates, fallback } = detectValleys([{ runId: "run", points }]);
    expect(candidates).toEqual([]);
    expect(fallback?.x).toBe(1000); // edge minimum of the pure trend
  });

  it("ranks a perfect-descale needle over a shallower multi-kernel consensus", () => {
    // Screenshot case: several kernels dip together near 866 with ~1e-4 error,
    // while bilinear spikes to 0 at 864. Consensus must not beat perfect descale.
    const crowd = ["bicubic", "spline16", "spline36", "spline64", "lanczos3"].map(
      (runId, seed) => ({
        runId,
        points: syntheticSeries([{ x: 866, depth: 0.2, width: 2 }], {
          noise: 0.01,
          seed: seed + 1,
        }),
      }),
    );
    const bilinearPoints = syntheticSeries([{ x: 866, depth: 0.2, width: 2 }], {
      noise: 0.01,
      seed: 99,
    }).map((point) =>
      point.x === 864
        ? { ...point, metric: 0, key: "bilinear-864" }
        : point,
    );
    const { candidates } = detectValleys([
      ...crowd,
      { runId: "bilinear", points: bilinearPoints },
    ]);
    expect(candidates[0]?.height).toBe(864);
    expect(candidates[0]?.deepest.runId).toBe("bilinear");
    expect(candidates[0]?.deepest.metric).toBeLessThanOrEqual(1e-6);
  });

  it("scopes both candidates and fallback to the points it is given", () => {
    // Simulates the panel's zoom-window filtering: only x in [800, 900].
    const points = syntheticSeries([{ x: 870, depth: 0.4, width: 3 }], {
      noise: 0.01,
    }).filter((p) => p.x >= 800 && p.x <= 900);
    const { candidates, fallback } = detectValleys([{ runId: "run", points }]);
    expect(candidates[0]?.height).toBe(870);
    expect(fallback?.x).toBeGreaterThanOrEqual(800);
    expect(fallback?.x).toBeLessThanOrEqual(900);
  });
});
