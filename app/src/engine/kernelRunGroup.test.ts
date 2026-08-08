import { describe, expect, it } from "vitest";
import {
  defaultKernelDraft,
  geometryGroupKey,
  resolveKernelCandidates,
} from "./kernelDraft";
import { materializeKernelRunGroup, planKernelRunGroup } from "./kernelRunGroup";
import type { EngineEnvelope } from "./types";
import type { MetricSpec } from "./protocol";

const metric: MetricSpec = {
  cropLeft: 10,
  cropRight: 10,
  cropTop: 10,
  cropBottom: 10,
  pixelExclusionThreshold: 0.015,
  pNorm: 1,
};

const capabilities: EngineEnvelope = {
  path: "/bin/engine",
  payload: {
    schema_version: 2,
    engine: "getnative-engine",
    version: "0.1.0",
    commands: { capabilities: true, geometry: true, analyze: false },
    kernels: [
      { id: "bilinear", parameters: {} },
      { id: "bicubic", parameters: {} },
      { id: "lanczos", parameters: {} },
      { id: "spline16", parameters: {} },
      { id: "spline36", parameters: {} },
      { id: "spline64", parameters: {} },
    ],
    backends: [],
    profiles: [{ id: "muf-d278cd3", default_crop: 5 }],
  },
};

function draft(): ReturnType<typeof defaultKernelDraft> {
  return defaultKernelDraft(capabilities, metric, "muf-d278cd3", "raw", "auto");
}

const sample = {
  id: "smp_1",
  label: "frame 12",
  sourceId: "src_1",
  sourceFingerprint: "fp-a",
  streamIndex: 0,
  frameIndex: 12,
  included: true,
};

const sources = {
  src_1: {
    id: "src_1",
    path: "/tmp/clip.mkv",
    fingerprint: "fp-a",
    state: "ready",
    width: 1920,
    height: 1080,
  },
};

const geometry = {
  mode: "standard" as const,
  activeWidth: 1920,
  activeHeight: 1080,
  canvasWidth: 1280,
  canvasHeight: 720,
  srcLeft: 0,
  srcTop: 0,
  srcWidth: 1920,
  srcHeight: 1080,
  baseWidth: null,
  baseHeight: 720,
  parity: null,
};

function resolvedGeometries(d: ReturnType<typeof draft>) {
  const key = geometryGroupKey({
    sourceWidth: 1920,
    sourceHeight: 1080,
    baseHeight: d.baseHeight,
    baseWidth: d.baseWidth,
    profileId: d.profileId,
  });
  return { [key]: geometry };
}

describe("resolveKernelCandidates", () => {
  it("emits preset families in the fixed deterministic order", () => {
    const result = resolveKernelCandidates(draft(), capabilities);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.candidates.map((kernel) => kernel.id)).toEqual([
      "bilinear",
      "bicubic",
      "spline16",
      "spline36",
      "spline64",
      "lanczos",
    ]);
    expect(result.candidates[1]?.parameters).toEqual({ b: 0, c: 0.5 });
    expect(result.candidates[5]?.parameters).toEqual({ taps: 3 });
  });

  it("expands the Bicubic grid b-outer, c-inner with exact decimal strings", () => {
    const d = { ...draft(), scanMode: "bicubic_grid" as const, bStop: "0.5", bStep: "0.5", cStop: "0.5", cStep: "0.5" };
    const result = resolveKernelCandidates(d, capabilities);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.candidates.map((kernel) => kernel.parameters)).toEqual([
      { b: "0", c: "0" },
      { b: "0", c: "0.5" },
      { b: "0.5", c: "0" },
      { b: "0.5", c: "0.5" },
    ]);
  });

  it("rejects invalid bicubic parameters and empty family selection", () => {
    const bad = { ...draft(), bicubicB: "abc" };
    expect(resolveKernelCandidates(bad, capabilities).ok).toBe(false);
    const none = draft();
    none.families = {
      bilinear: false,
      bicubic: false,
      spline16: false,
      spline36: false,
      spline64: false,
      lanczos: false,
    };
    const result = resolveKernelCandidates(none, capabilities);
    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.reason).toBe("no_kernels");
  });
});

describe("planKernelRunGroup", () => {
  it("builds one member run per sample with fixed geometry and full candidate list", () => {
    const d = draft();
    const result = planKernelRunGroup({
      draft: d,
      samples: [sample],
      sourcesById: sources,
      geometries: resolvedGeometries(d),
      capabilities,
      nowMs: 1,
      requestIdPrefix: "test",
    });
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.plan.groupType).toBe("single_kernel");
    expect(result.plan.members).toHaveLength(1);
    expect(result.plan.members[0]?.kernels.length).toBe(6);
    expect(result.plan.members[0]?.geometry.canvasHeight).toBe(720);
    expect(result.plan.members[0]?.request.mode).toBe("kernel");
    expect(result.plan.workEstimate).toBe(6);

    const materialized = materializeKernelRunGroup({
      plan: result.plan,
      idFactory: () => "xyz",
      nowIso: "2026-08-07T00:00:00Z",
    });
    expect(materialized.runs).toHaveLength(1);
    expect(materialized.runs[0]?.runType).toBe("kernel");
    expect(materialized.runs[0]?.result).toBeNull();
    expect(materialized.runs[0]?.total).toBe(6);
  });

  it("blocks when geometry is unresolved or the source is stale", () => {
    const d = draft();
    const unresolved = planKernelRunGroup({
      draft: d,
      samples: [sample],
      sourcesById: sources,
      geometries: {},
      capabilities,
    });
    expect(unresolved.ok).toBe(false);
    if (!unresolved.ok) expect(unresolved.reason).toBe("geometry_unresolved");

    const stale = planKernelRunGroup({
      draft: d,
      samples: [{ ...sample, sourceFingerprint: "fp-changed" }],
      sourcesById: sources,
      geometries: resolvedGeometries(d),
      capabilities,
    });
    expect(stale.ok).toBe(false);
    if (!stale.ok) expect(stale.reason).toBe("sample_fingerprint_stale");
  });

  it("rejects a single-candidate scan as too small for a kernel run", () => {
    const d = draft();
    d.families = {
      bilinear: true,
      bicubic: false,
      spline16: false,
      spline36: false,
      spline64: false,
      lanczos: false,
    };
    const result = planKernelRunGroup({
      draft: d,
      samples: [sample],
      sourcesById: sources,
      geometries: resolvedGeometries(d),
      capabilities,
    });
    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.reason).toBe("kernel_list_too_small");
  });
});

describe("extractKernelResultRows (worker v1.1 payload)", () => {
  it("reads the candidates array with kernel echoes in request order", () => {
    const { extractKernelResultRows } = await_import();
    const payload = {
      mode: "kernel",
      candidate: "720",
      candidates: [
        { id: "0", error: 5.1, kernel: { id: "bilinear" } },
        { id: "1", error: 0.4, kernel: { id: "bicubic", b: 0, c: 0.5 } },
        { id: "2", error: 0.4, kernel: { id: "bicubic", b: 0, c: 1 } },
      ],
      telemetry: { plan_cache_hits: 2 },
    };
    const rows = extractKernelResultRows(payload);
    expect(rows).toEqual([
      { kernelId: "bilinear", parameters: {}, metric: 5.1 },
      { kernelId: "bicubic", parameters: { b: 0, c: 0.5 }, metric: 0.4 },
      { kernelId: "bicubic", parameters: { b: 0, c: 1 }, metric: 0.4 },
    ]);
    expect(extractKernelResultRows(null)).toBeNull();
    expect(extractKernelResultRows({ candidates: "bogus" })).toBeNull();
  });
});

// Hoisted import helper keeps the appended block self-contained.
import { extractKernelResultRows as _extractKernelResultRows } from "./kernelRunGroup";
function await_import() {
  return { extractKernelResultRows: _extractKernelResultRows };
}
