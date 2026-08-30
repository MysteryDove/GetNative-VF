/**
 * Local-valley detection for getnative-style relative-error curves.
 *
 * A height-scan error curve often rides on a monotonic trend, so a plain
 * global minimum lands on the scan edge instead of the true native-height
 * dip. Detection therefore works per series in the log10 domain:
 *   1. light moving-average smoothing to suppress point noise,
 *   2. strict local minima on the smoothed curve,
 *   3. windowed prominence filter (depth below the surrounding local maxima),
 * then clusters the surviving valleys across runs (sample x kernel) so
 * heights corroborated by several series rank first.
 *
 * Perfect descale (error <= 1e-6) always outranks consensus: a single-point
 * needle to ~0 is the native kernel even when neighboring kernels dip
 * together a couple of pixels away.
 */

/** getnative convention: error <= 1e-6 means the candidate perfectly descales. */
export const PERFECTLY_DESCALE_THRESHOLD = 1e-6;

export type ValleySeriesPoint = {
  x: number;
  metric: number;
  /** Stable point key, e.g. `${runId}-${height}` — carried through for plot highlighting. */
  key?: string;
};

export type ValleySeries = {
  runId: string;
  points: ValleySeriesPoint[];
};

export type ValleyMember = {
  runId: string;
  key?: string;
  x: number;
  metric: number;
  /** Valley depth in log10 decades below its local surroundings. */
  prominence: number;
};

export type ValleyCandidate = {
  /** Height of the deepest member (an actually-measured grid point). */
  height: number;
  /** Sum of member prominences; more corroborating runs → higher score. */
  score: number;
  /** Distinct runs contributing a valley to this cluster. */
  runCount: number;
  /** Lowest-metric member — drives verdict metric/kernel and plot bestKey. */
  deepest: ValleyMember;
  members: ValleyMember[];
};

export type ValleyDetection = {
  /** Clusters sorted by score (best first), capped at `maxCandidates`. */
  candidates: ValleyCandidate[];
  /** Global minimum within scope, for honest display when no valley stands out. */
  fallback: { x: number; metric: number; runId: string; key?: string } | null;
};

export type ValleyDetectOptions = {
  /** Minimum prominence in log10 decades (0.1 ≈ 26% relative depth). */
  minProminence?: number;
  /** Moving-average window in points, used only for detection (forced odd). */
  smoothWindow?: number;
  /** Half-width in points of the prominence horizon; defaults to ~n/40. */
  horizonWindow?: number;
  /** Heights closer than this merge into one consensus cluster. */
  clusterTolerance?: number;
  maxCandidates?: number;
};

const LOG_FLOOR = 1e-12;

function movingAverage(values: number[], window: number): number[] {
  const n = values.length;
  let w = Math.max(1, Math.min(Math.round(window), n));
  if (w % 2 === 0) w = Math.max(1, w - 1);
  const half = Math.floor(w / 2);
  const prefix = new Array<number>(n + 1);
  prefix[0] = 0;
  for (let i = 0; i < n; i += 1) prefix[i + 1] = prefix[i] + values[i];
  const out = new Array<number>(n);
  for (let i = 0; i < n; i += 1) {
    const from = Math.max(0, i - half);
    const to = Math.min(n, i + half + 1);
    out[i] = (prefix[to] - prefix[from]) / (to - from);
  }
  return out;
}

/**
 * Windowed prominence of a local minimum at `i`, in log10 decades: the
 * shallower of the two windowed maxima flanking it, minus its own value.
 * The horizon stops early at any deeper valley, mirroring SciPy prominence.
 */
function windowedProminence(ys: number[], i: number, horizon: number): number {
  const n = ys.length;
  let leftMax = -Infinity;
  for (let j = i - 1; j >= Math.max(0, i - horizon); j -= 1) {
    if (ys[j] <= ys[i]) break;
    if (ys[j] > leftMax) leftMax = ys[j];
  }
  let rightMax = -Infinity;
  for (let j = i + 1; j < Math.min(n, i + horizon + 1); j += 1) {
    if (ys[j] <= ys[i]) break;
    if (ys[j] > rightMax) rightMax = ys[j];
  }
  const bound = Math.min(leftMax, rightMax);
  return Number.isFinite(bound) ? bound - ys[i] : 0;
}

/** Prominent local minima of one series, in ascending x order. */
export function findSeriesValleys(
  series: ValleySeries,
  options: ValleyDetectOptions = {},
): ValleyMember[] {
  const points = [...series.points]
    .filter((point) => Number.isFinite(point.x) && Number.isFinite(point.metric))
    .sort((a, b) => a.x - b.x);
  const n = points.length;
  if (n < 3) return [];
  const minProminence = options.minProminence ?? 0.1;
  const horizon =
    options.horizonWindow ?? Math.min(200, Math.max(10, Math.round(n / 40)));
  const requestedSmooth = options.smoothWindow ?? (n >= 5 ? 5 : 3);
  let smooth = Math.max(1, Math.min(Math.round(requestedSmooth), n));
  if (smooth % 2 === 0) smooth = Math.max(1, smooth - 1);
  const half = Math.floor(smooth / 2);
  const ys = movingAverage(
    points.map((point) => Math.log10(Math.max(point.metric, LOG_FLOOR))),
    smooth,
  );
  const out: ValleyMember[] = [];
  for (let i = 1; i < n - 1; i += 1) {
    if (!(ys[i] < ys[i - 1] && ys[i] <= ys[i + 1])) continue;
    const prominence = windowedProminence(ys, i, horizon);
    if (prominence < minProminence) continue;
    // Smoothing can shift a needle by a couple of samples; report the raw
    // minimum in that window so the verdict height/metric match the plot.
    let best = i;
    for (let j = Math.max(0, i - half); j <= Math.min(n - 1, i + half); j += 1) {
      if (points[j].metric < points[best].metric) best = j;
    }
    out.push({
      runId: series.runId,
      key: points[best].key,
      x: points[best].x,
      metric: points[best].metric,
      prominence,
    });
  }
  return out;
}

/** Merge per-series valleys by height proximity into ranked consensus candidates. */
export function clusterValleys(
  members: ValleyMember[],
  options: ValleyDetectOptions = {},
): ValleyCandidate[] {
  const tolerance = options.clusterTolerance ?? 1;
  const sorted = [...members].sort((a, b) => a.x - b.x);
  const clusters: ValleyMember[][] = [];
  for (const member of sorted) {
    const cluster = clusters[clusters.length - 1];
    if (cluster && member.x - cluster[cluster.length - 1].x <= tolerance) {
      cluster.push(member);
    } else {
      clusters.push([member]);
    }
  }
  return clusters
    .map((cluster) => {
      const deepest = cluster.reduce((best, m) => (m.metric < best.metric ? m : best));
      return {
        height: deepest.x,
        score: cluster.reduce((sum, m) => sum + m.prominence, 0),
        runCount: new Set(cluster.map((m) => m.runId)).size,
        deepest,
        members: cluster,
      };
    })
    .sort((a, b) => {
      const aPerfect = a.deepest.metric <= PERFECTLY_DESCALE_THRESHOLD ? 1 : 0;
      const bPerfect = b.deepest.metric <= PERFECTLY_DESCALE_THRESHOLD ? 1 : 0;
      if (aPerfect !== bPerfect) return bPerfect - aPerfect;
      if (aPerfect) {
        return (
          a.deepest.metric - b.deepest.metric ||
          b.runCount - a.runCount ||
          a.height - b.height
        );
      }
      return b.score - a.score || b.runCount - a.runCount || a.height - b.height;
    });
}

/**
 * Full pipeline: per-series valleys → cross-run consensus clusters, plus the
 * global minimum as an explicit fallback for trend-only curves where no
 * local valley stands out.
 */
/** Lowest raw point at or below the perfect-descale threshold, per series. */
function perfectDescaleMembers(seriesList: ValleySeries[]): ValleyMember[] {
  const out: ValleyMember[] = [];
  for (const series of seriesList) {
    let best: ValleySeriesPoint | null = null;
    for (const point of series.points) {
      if (!Number.isFinite(point.x) || !Number.isFinite(point.metric)) continue;
      if (point.metric > PERFECTLY_DESCALE_THRESHOLD) continue;
      if (!best || point.metric < best.metric) best = point;
    }
    if (best) {
      out.push({
        runId: series.runId,
        key: best.key,
        x: best.x,
        metric: best.metric,
        prominence: 1,
      });
    }
  }
  return out;
}

export function detectValleys(
  seriesList: ValleySeries[],
  options: ValleyDetectOptions = {},
): ValleyDetection {
  const members = [
    ...seriesList.flatMap((series) => findSeriesValleys(series, options)),
    ...perfectDescaleMembers(seriesList),
  ];
  const candidates = clusterValleys(members, options).slice(0, options.maxCandidates ?? 5);
  let fallback: ValleyDetection["fallback"] = null;
  for (const series of seriesList) {
    for (const point of series.points) {
      if (!Number.isFinite(point.x) || !Number.isFinite(point.metric)) continue;
      if (!fallback || point.metric < fallback.metric) {
        fallback = { x: point.x, metric: point.metric, runId: series.runId, key: point.key };
      }
    }
  }
  return { candidates, fallback };
}
