import { describe, expect, it } from "vitest";
import {
  addBicubicGridToScanList,
  addBlurParameters,
  addKernelToScanList,
  bicubicRefFromDraft,
  clearScanList,
  defaultKernelDraft,
  lanczosRefsFromDraft,
  lanczosTapsRange,
  normalizeKernelRef,
  removeKernelFromScanList,
  resolveKernelCandidates,
  withAddBlurParams,
} from "./kernelDraft";
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

function draft() {
  return defaultKernelDraft(metric, "muf-d278cd3", "raw", "auto");
}

describe("kernel scan list", () => {
  it("seeds the six preset families and honors the base override", () => {
    const seeded = draft();
    expect(seeded.scanList.map((kernel) => kernel.id)).toEqual([
      "bilinear",
      "bicubic",
      "spline16",
      "spline36",
      "spline64",
      "lanczos",
    ]);
    expect(seeded.baseHeight).toBe("720");
    expect(seeded.baseWidth).toBe("");
    expect(seeded.addBlur).toBe("1");

    const carried = defaultKernelDraft(metric, "muf-d278cd3", "raw", "auto", {
      baseHeight: "810",
      baseWidth: "1440",
    });
    expect(carried.baseHeight).toBe("810");
    expect(carried.baseWidth).toBe("1440");
  });

  it("appends new kernels and rejects signature duplicates", () => {
    const start = clearScanList(draft());
    const first = addKernelToScanList(start, { id: "bicubic", parameters: { b: 0, c: 0.5 } });
    expect(first.added).toBe(true);
    const dupe = addKernelToScanList(first.draft, {
      id: "bicubic",
      parameters: { b: 0, c: 0.5 },
    });
    expect(dupe.added).toBe(false);
    expect(dupe.draft.scanList).toHaveLength(1);
    // Same id, different parameters is a different candidate.
    const variant = addKernelToScanList(first.draft, {
      id: "bicubic",
      parameters: { b: 0, c: 1 },
    });
    expect(variant.added).toBe(true);
  });

  it("normalizes numeric strings so grid entries dedup against manual entries", () => {
    expect(normalizeKernelRef({ id: "bicubic", parameters: { b: "0", c: "0.5" } })).toEqual({
      id: "bicubic",
      parameters: { b: 0, c: 0.5 },
    });
    const manual = addKernelToScanList(clearScanList(draft()), {
      id: "bicubic",
      parameters: { b: 0, c: 0.5 },
    });
    const gridStyle = addKernelToScanList(manual.draft, {
      id: "bicubic",
      parameters: { b: "0.0", c: "0.50" },
    });
    expect(gridStyle.added).toBe(false);
  });

  it("adds the bicubic grid with b-outer/c-inner order and skip counts", () => {
    const base = clearScanList({
      ...draft(),
      bStop: "0.4",
      bStep: "0.2",
      cStop: "0",
      cStep: "0.5",
    });
    const first = addBicubicGridToScanList(base);
    expect(first.ok).toBe(true);
    if (!first.ok) return;
    expect(first.added).toBe(3);
    expect(first.skipped).toBe(0);
    expect(first.draft.scanList.map((kernel) => kernel.parameters)).toEqual([
      { b: 0, c: 0 },
      { b: 0.2, c: 0 },
      { b: 0.4, c: 0 },
    ]);
    // Re-adding the same grid skips everything.
    const second = addBicubicGridToScanList(first.draft);
    expect(second.ok).toBe(true);
    if (!second.ok) return;
    expect(second.added).toBe(0);
    expect(second.skipped).toBe(3);
  });

  it("passes through grid range errors", () => {
    const bad = clearScanList({ ...draft(), bStep: "0" });
    const result = addBicubicGridToScanList(bad);
    expect(result.ok).toBe(false);
  });

  it("builds lanczos refs from the taps selection, ascending and deduped", () => {
    const d = { ...draft(), lanczosTapsSelection: [5, 2, 3, 2] };
    expect(lanczosRefsFromDraft(d).map((kernel) => kernel.parameters)).toEqual([
      { taps: 2 },
      { taps: 3 },
      { taps: 5 },
    ]);
  });

  it("reads the lanczos taps range from engine metadata with a 2..6 fallback", () => {
    expect(lanczosTapsRange(null)).toEqual({ min: 2, max: 6 });
    const capabilities = {
      path: "/bin/engine",
      payload: {
        schema_version: 2,
        engine: "getnative-engine",
        version: "0.1.0",
        commands: { capabilities: true, geometry: true, analyze: false },
        kernels: [
          {
            id: "lanczos",
            parameters: { kind: "integer_taps", gui_min: 1, gui_max: 8 },
          },
        ],
        backends: [],
        profiles: [],
      },
    } satisfies EngineEnvelope;
    expect(lanczosTapsRange(capabilities)).toEqual({ min: 1, max: 8 });
  });

  it("removes entries by index and clears the list", () => {
    const d = draft();
    const removed = removeKernelFromScanList(d, 1);
    expect(removed.scanList).toHaveLength(5);
    expect(removed.scanList.map((kernel) => kernel.id)).not.toContain("bicubic");
    expect(clearScanList(d).scanList).toHaveLength(0);
  });

  it("validates bicubic add-form inputs", () => {
    expect(bicubicRefFromDraft(draft())).toEqual({
      id: "bicubic",
      parameters: { b: 0, c: 0.5 },
    });
    expect(bicubicRefFromDraft({ ...draft(), bicubicB: "-1" })).toBeNull();
    expect(bicubicRefFromDraft({ ...draft(), bicubicC: "" })).toBeNull();
  });

  it("omits default blur and bakes non-default blur into added kernels", () => {
    expect(addBlurParameters(draft())).toEqual({ ok: true, parameters: {} });
    expect(addBlurParameters({ ...draft(), addBlur: "1.25" })).toEqual({
      ok: true,
      parameters: { blur: 1.25 },
    });
    expect(addBlurParameters({ ...draft(), addBlur: "0" }).ok).toBe(false);
    expect(addBlurParameters({ ...draft(), addBlur: "" }).ok).toBe(false);

    const plain = withAddBlurParams(draft(), { id: "bilinear", parameters: {} });
    expect(plain).toEqual({ id: "bilinear", parameters: {} });
    const widened = withAddBlurParams(
      { ...draft(), addBlur: "1.25" },
      { id: "bilinear", parameters: {} },
    );
    expect(widened).toEqual({ id: "bilinear", parameters: { blur: 1.25 } });
    expect(withAddBlurParams({ ...draft(), addBlur: "0" }, { id: "spline36", parameters: {} })).toBeNull();

    const start = clearScanList(draft());
    const first = addKernelToScanList(
      start,
      withAddBlurParams({ ...start, addBlur: "1.25" }, { id: "bilinear", parameters: {} })!,
    );
    expect(first.added).toBe(true);
    expect(first.draft.scanList[0]).toEqual({ id: "bilinear", parameters: { blur: 1.25 } });
    const sameBlur = addKernelToScanList(first.draft, {
      id: "bilinear",
      parameters: { blur: 1.25 },
    });
    expect(sameBlur.added).toBe(false);
    const differentBlur = addKernelToScanList(first.draft, { id: "bilinear", parameters: {} });
    expect(differentBlur.added).toBe(true);
  });

  it("applies add-form blur to the bicubic grid", () => {
    const base = clearScanList({
      ...draft(),
      addBlur: "1.5",
      bStop: "0",
      bStep: "0.2",
      cStop: "0",
      cStep: "0.5",
    });
    const result = addBicubicGridToScanList(base);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.added).toBe(1);
    expect(result.draft.scanList[0]?.parameters).toEqual({ b: 0, c: 0, blur: 1.5 });

    const invalid = addBicubicGridToScanList({ ...base, addBlur: "0" });
    expect(invalid.ok).toBe(false);
  });

  it("filters the scan list to engine-reported kernels at resolve time", () => {
    const capabilities = {
      path: "/bin/engine",
      payload: {
        schema_version: 2,
        engine: "getnative-engine",
        version: "0.1.0",
        commands: { capabilities: true, geometry: true, analyze: false },
        kernels: [{ id: "bilinear", parameters: {} }],
        backends: [],
        profiles: [],
      },
    } satisfies EngineEnvelope;
    const result = resolveKernelCandidates(draft(), capabilities);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.candidates.map((kernel) => kernel.id)).toEqual(["bilinear"]);
  });
});
