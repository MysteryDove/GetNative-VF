import { describe, expect, it } from "vitest";
import {
  formatParameterValue,
  hasFractionalPart,
  padDecimals,
  parseNumberInput,
} from "./numberInput";
import {
  applyPreset,
  convertScanRange,
  defaultHeightDraft,
  fixedKernelsForDraft,
  invalidKernelBlur,
  invalidKernelParameterNames,
  missingFractionalBaseAxis,
} from "./heightDraft";
import {
  addBicubicGridToScanList,
  addBlurValues,
  clearScanList,
  defaultKernelDraft,
  withAddBlurVariants,
} from "./kernelDraft";
import { geometryForSource, resolveGeometryValues } from "./geometry";
import { buildKernelResultRows, kernelRunTransfer } from "./kernelRunGroup";
import { createRecipe } from "../project/recipe";
import { buildBicubicGrid, heatStep, rankKernelsAcrossSamples } from "./kernelHeatmap";
import { analyzeViewState, restoreDraft } from "../project/analyzeView";
import { defaultVerifyDraft, planVerifyRunGroup } from "./verifyPlan";
import { buildSeriesTable, heightRunConfig, planHeightRunGroup } from "./runGroupPlan";

describe("number input", () => {
  it("parses decimals and fractions, rejecting malformed text", () => {
    expect(parseNumberInput("0.5")).toBe(0.5);
    expect(parseNumberInput("1/3")).toBeCloseTo(1 / 3, 15);
    expect(parseNumberInput("-2/3")).toBeCloseTo(-2 / 3, 15);
    expect(parseNumberInput("1/0")).toBeNull();
    expect(parseNumberInput("abc")).toBeNull();
    expect(parseNumberInput("")).toBeNull();
  });

  it("renders thirds as fractions and leaves other values alone", () => {
    expect(formatParameterValue(1 / 3)).toBe("1/3");
    expect(formatParameterValue(2 / 3)).toBe("2/3");
    expect(formatParameterValue(0.333)).toBe("0.333");
    expect(formatParameterValue(1)).toBe("1");
    expect(formatParameterValue(0.5)).toBe("0.5");
  });

  it("pads candidates to the scan precision without re-rounding", () => {
    expect(padDecimals("843.7", 2)).toBe("843.70");
    expect(padDecimals("844", 1)).toBe("844.0");
    expect(padDecimals("843.685", 2)).toBe("843.685");
    expect(hasFractionalPart("1.0")).toBe(false);
    expect(hasFractionalPart("0.1")).toBe(true);
  });
});

describe("decimal scans and the base", () => {
  it("flags a broad scan with a decimal step and an integer base", () => {
    const draft = { ...defaultHeightDraft(null), preset: "integer_coarse" as const, step: "0.1" };
    expect(missingFractionalBaseAxis(draft)).toBe("height");
    expect(missingFractionalBaseAxis({ ...draft, baseHeightMode: "even" })).toBeNull();
    expect(missingFractionalBaseAxis({ ...draft, step: "1" })).toBeNull();
  });

  it("moves to an even base when entering Refine and keeps the entered range on Broad", () => {
    const refined = applyPreset(defaultHeightDraft(null), "fractional_refine");
    expect(refined.baseHeightMode).toBe("even");
    const broad = applyPreset({ ...refined, start: "720", stop: "960", step: "0.1" }, "integer_coarse");
    expect([broad.start, broad.stop, broad.step]).toEqual(["720", "960", "0.1"]);
  });

  it("converts a scan range across axes by aspect ratio", () => {
    expect(
      convertScanRange({ start: "500", stop: "1000", refineSelected: "720" }, 16 / 9),
    ).toEqual({ start: "889", stop: "1778", refineSelected: "1280" });
    expect(convertScanRange({ start: "", stop: "x", refineSelected: "720" }, 2).start).toBe("");
  });
});

describe("fraction kernel parameters", () => {
  it("resolves typed b/c fractions to numbers for the engine", () => {
    const draft = {
      ...defaultHeightDraft(null),
      kernelId: "bicubic",
      kernelParameters: { b: "1/3", c: "1/3" },
    };
    expect(invalidKernelParameterNames(draft.kernelParameters)).toEqual([]);
    const [primary] = fixedKernelsForDraft(draft, null);
    expect(primary.parameters.b).toBeCloseTo(1 / 3, 15);
    expect(invalidKernelParameterNames({ b: "1/", c: "0.5" })).toEqual(["b"]);
  });

  it("steps a Bicubic grid by 1/3 and lands on the endpoints", () => {
    const base = defaultKernelDraft(defaultHeightDraft(null).metric, "p", "raw", "auto");
    const result = addBicubicGridToScanList(
      clearScanList({ ...base, bStep: "1/3", cStart: "0", cStop: "0", cStep: "1" }),
    );
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.draft.scanList.map((kernel) => formatParameterValue(kernel.parameters.b))).toEqual(
      ["0", "1/3", "2/3", "1"],
    );
  });
});

describe("kernel blur limit", () => {
  it("accepts blur up to 16 and rejects anything above or non-positive", () => {
    expect(invalidKernelBlur({ blur: "1" })).toBe(false);
    expect(invalidKernelBlur({ blur: 16 })).toBe(false);
    expect(invalidKernelBlur({ blur: "16.5" })).toBe(true);
    expect(invalidKernelBlur({ blur: "10000" })).toBe(true);
    expect(invalidKernelBlur({ blur: "0" })).toBe(true);
    expect(invalidKernelBlur({})).toBe(false);
  });
});

describe("height run config", () => {
  it("reads back the base mode and precision a Run was measured with", () => {
    const run = {
      inputSnapshot: {
        kernel: { id: "bicubic", parameters: { b: 0, c: 0.5 } },
        heightGrid: { start: "500", stop: "1000", step: "0.05" },
        request: { axisMode: "h_plus_w", baseHeight: "1000", baseWidth: null },
      },
    };
    const fromRequest = heightRunConfig(run);
    expect(fromRequest.decimals).toBe(2);
    expect(fromRequest.baseHeightMode).toBe("even");
    expect(fromRequest.baseWidthMode).toBe("integer");
    expect(
      heightRunConfig(run, { intentSnapshot: { baseHeightMode: "odd", baseWidthMode: "even" } })
        .baseHeightMode,
    ).toBe("odd");
  });
});

describe("blur sweep", () => {
  const base = defaultKernelDraft(defaultHeightDraft(null).metric, "p", "raw", "auto");

  it("stays a single blur until a stop value is entered", () => {
    expect(addBlurValues(base)).toEqual({ ok: true, values: [1] });
    expect(addBlurValues({ ...base, addBlur: "1.2" })).toEqual({ ok: true, values: [1.2] });
  });

  it("expands start..stop inclusively and omits blur=1 from the kernel identity", () => {
    const sweep = { ...base, addBlur: "0.9", blurStop: "1.1", blurStep: "0.05" };
    expect(addBlurValues(sweep)).toEqual({ ok: true, values: [0.9, 0.95, 1, 1.05, 1.1] });
    const variants = withAddBlurVariants(sweep, { id: "bicubic", parameters: { b: 0, c: 0.5 } });
    expect(variants?.map((kernel) => kernel.parameters.blur)).toEqual(
      [0.9, 0.95, undefined, 1.05, 1.1],
    );
  });

  it("rejects sweeps that leave the valid blur range or have a bad step", () => {
    expect(addBlurValues({ ...base, addBlur: "1", blurStop: "17", blurStep: "1" }).ok).toBe(false);
    expect(addBlurValues({ ...base, addBlur: "0", blurStop: "1", blurStep: "0.5" }).ok).toBe(false);
    expect(addBlurValues({ ...base, addBlur: "1", blurStop: "2", blurStep: "0" }).ok).toBe(false);
    expect(addBlurValues({ ...base, addBlur: "2", blurStop: "1", blurStep: "0.1" }).ok).toBe(false);
  });

  it("multiplies a Bicubic grid by the blur values", () => {
    const result = addBicubicGridToScanList(clearScanList({
      ...base, bStop: "0", cStart: "0.5", cStop: "0.5", cStep: "1",
      addBlur: "0.9", blurStop: "1.1", blurStep: "0.1",
    }));
    expect(result.ok && result.added).toBe(3);
  });
});

describe("non-proportional geometry", () => {
  it("keeps an independent src width on the Recipe's own source shape", () => {
    const anamorphic = resolveGeometryValues({
      sourceWidth: 1920, sourceHeight: 1080,
      srcWidth: 1440.5, srcHeight: 843.75, baseWidth: 1442, baseHeight: 844,
    });
    const same = geometryForSource(anamorphic, "h_plus_w", 1920, 1080);
    expect(same.srcWidth).toBe(1440.5);
    expect(same.srcHeight).toBe(843.75);
    // A different source shape still follows that source's aspect ratio.
    const other = geometryForSource(anamorphic, "h_plus_w", 1280, 720);
    expect(other.srcWidth).toBeCloseTo(1280 * 843.75 / 720, 9);
  });
});

describe("transfer curve", () => {
  const run = (transfer?: string) => ({
    id: `r-${transfer ?? "none"}`, runType: "kernel", status: "completed", runGroupId: null,
    sampleId: "s", sourceId: "src", createdAt: "", updatedAt: "",
    inputSnapshot: {
      kernels: [{ id: "bilinear", parameters: {} }],
      request: transfer ? { transfer } : {},
    },
    result: { candidates: [{ id: "0", error: 1e-4, kernel: { id: "bilinear" } }] },
    errorCode: null, errorMessage: null, completed: 1, total: 1,
  });
  const state = { samplesById: {} } as never;

  it("reads the curve a kernel Run was measured under", () => {
    expect(kernelRunTransfer(run() as never)).toBe("none");
    expect(kernelRunTransfer(run("bt1886") as never)).toBe("bt1886");
  });

  it("shows only Runs measured with the selected curve", () => {
    const runs = [run(), run("bt1886"), run("srgb")] as never[];
    const encoded = buildKernelResultRows(runs, state, "", "none");
    expect(encoded.rows.map((row) => row.runId)).toEqual(["r-none"]);
    expect(encoded.incompatibleCount).toBe(2);
    const linear = buildKernelResultRows(runs, state, "", "bt1886");
    expect(linear.rows.map((row) => row.runId)).toEqual(["r-bt1886"]);
  });
});

describe("transfer curve in the Resolution Test", () => {
  const sources = { src: { id: "src", path: "a.mkv", state: "ready", width: 1920, height: 1080 } };
  const samples = [{ id: "s", sourceId: "src", included: true }];

  it("sends the curve with each member request only when one is selected", () => {
    const plain = planHeightRunGroup({
      draft: defaultHeightDraft(null), samples, sourcesById: sources, capabilities: null,
    });
    const linear = planHeightRunGroup({
      draft: { ...defaultHeightDraft(null), transfer: "bt1886" },
      samples, sourcesById: sources, capabilities: null,
    });
    expect(plain.ok && "transfer" in plain.plan.members[0].request).toBe(false);
    expect(linear.ok && linear.plan.members[0].request.transfer).toBe("bt1886");
  });

  it("reads the curve back and shows only Runs measured with the selected one", () => {
    const run = (id: string, transfer?: string) => ({
      id, runType: "height", status: "completed", runGroupId: null, sampleId: null, sourceId: "src",
      createdAt: "", updatedAt: "", errorCode: null, errorMessage: null, completed: 1, total: 1,
      inputSnapshot: {
        kernel: { id: "bilinear", parameters: {} },
        heightGrid: { start: "700", stop: "800", step: "1" },
        request: { axisMode: "h_plus_w", ...(transfer ? { transfer } : {}) },
      },
      result: { candidates: [{ id: "720", error: 1e-7 }] },
    });
    const runs = [run("plain"), run("linear", "bt1886")] as never[];
    expect(heightRunConfig(runs[1]).transfer).toBe("bt1886");
    expect(heightRunConfig(runs[0]).transfer).toBe("none");
    const state = { samplesById: {} } as never;
    const encoded = buildSeriesTable(runs, state, new Set(), "", "h_plus_w", "none");
    expect(encoded.seriesMeta.map((meta) => meta.runId)).toEqual(["plain"]);
    expect(encoded.incompatibleCount).toBe(1);
    const linear = buildSeriesTable(runs, state, new Set(), "", "h_plus_w", "bt1886");
    expect(linear.seriesMeta.map((meta) => meta.runId)).toEqual(["linear"]);
  });
});

describe("transfer curve on the Recipe and Check", () => {
  const sourcesById = {
    src: {
      id: "src", kind: "video", path: "a.mkv", state: "ready", width: 1920, height: 1080,
      fingerprint: "f", videoStreams: [{ index: 0 }], selectedStreamIndex: 0,
    },
  } as never;
  const recipe = (transfer?: string) => {
    const created = createRecipe(
      { recipesById: {}, project: { activeRecipeId: null } } as never,
      {
        name: "r",
        geometry: resolveGeometryValues({ sourceWidth: 1920, sourceHeight: 1080, srcWidth: 1280, srcHeight: 720 }),
        kernel: { id: "bilinear", parameters: {} },
        metric: defaultHeightDraft(null).metric,
        mathMode: "raw",
        ...(transfer ? { transfer: transfer as never } : {}),
      },
    );
    if (!created.ok) throw new Error("recipe");
    return created.recipe;
  };

  it("defaults to as-encoded and stores an applied curve", () => {
    expect(recipe().transfer).toBe("none");
    expect(recipe("bt1886").transfer).toBe("bt1886");
  });

  it("sends the Recipe's curve with the Check request only when it has one", () => {
    const plan = (item: ReturnType<typeof recipe>) => planVerifyRunGroup({
      draft: { ...defaultVerifyDraft(), sourceIds: ["src"] },
      recipe: item, sourcesById, capabilities: null,
    });
    const plain = plan(recipe());
    const linear = plan(recipe("bt1886"));
    expect(plain.ok && "transfer" in plain.plan.members[0].request).toBe(false);
    expect(linear.ok && linear.plan.members[0].request.transfer).toBe("bt1886");
  });

  it("refuses a linear-light Check on a worker that cannot apply the curve", () => {
    const worker = (verify_transfer: boolean) => ({
      payload: { kernels: [], backends: [], profiles: [], features: { verify_transfer } },
    }) as never;
    const plan = (item: ReturnType<typeof recipe>, capabilities: unknown) => planVerifyRunGroup({
      draft: { ...defaultVerifyDraft(), sourceIds: ["src"] },
      recipe: item, sourcesById, capabilities: capabilities as never,
    });
    const refused = plan(recipe("bt1886"), worker(false));
    expect(!refused.ok && refused.reason).toBe("transfer_unsupported");
    expect(plan(recipe("bt1886"), worker(true)).ok).toBe(true);
    expect(plan(recipe(), worker(false)).ok).toBe(true);
  });
});

describe("stored parameter drafts", () => {
  it("restores known fields of the right kind and ignores everything else", () => {
    const defaults = { start: "500", step: "1", axisMode: "h_plus_w", compareKernels: [] as unknown[], metric: { pNorm: 1 } };
    const restored = restoreDraft(defaults, {
      start: "720", step: 0.1, axisMode: "w_only", compareKernels: [{ id: "bilinear" }],
      metric: { pNorm: 4 }, unknownField: true,
    }, ["metric"]);
    expect(restored.start).toBe("720");
    expect(restored.step).toBe("1");                 // wrong kind is ignored
    expect(restored.axisMode).toBe("w_only");
    expect(restored.compareKernels).toEqual([{ id: "bilinear" }]);
    expect(restored.metric).toEqual({ pNorm: 1 });   // skipped key keeps the default
    expect("unknownField" in restored).toBe(false);
    expect(restoreDraft(defaults, null)).toBe(defaults);
  });

  it("reads drafts back from the project and tolerates older projects", () => {
    expect(analyzeViewState({ uiStateByRoute: {} } as never).heightDraft).toBeNull();
    const view = analyzeViewState({
      uiStateByRoute: { analyze: { heightDraft: { start: "720" }, kernelDraft: [] } },
    } as never);
    expect(view.heightDraft).toEqual({ start: "720" });
    expect(view.kernelDraft).toBeNull();
  });
});

describe("Kernel Search grid and ranking", () => {
  const row = (b: number, c: number, metric: number, sampleId = "s1", extra: Record<string, number> = {}) => ({
    candidateId: `${b}-${c}-${sampleId}`, runId: `r-${sampleId}`, sampleId, kernelId: "bicubic",
    parameters: { b, c, ...extra }, kernelLabel: `bicubic (b=${b}, c=${c})`, metric, sampleLabel: sampleId,
  });

  it("builds a b × c grid, averaging Samples and marking the lowest cell", () => {
    const rows = [0, 0.5, 1].flatMap((b) => [0, 0.5].flatMap((c) => [
      row(b, c, (b + 1) * (c + 1) * 1e-6, "s1"),
      row(b, c, (b + 1) * (c + 1) * 3e-6, "s2"),
    ]));
    const grid = buildBicubicGrid(rows);
    expect(grid?.bs).toEqual([0, 0.5, 1]);
    expect(grid?.cs).toEqual([0, 0.5]);
    expect(grid?.best.b).toBe(0);
    expect(grid?.best.metric).toBeCloseTo(2e-6, 12);
    expect(grid?.best.count).toBe(2);
    expect(grid?.best.keys[0]).toBe("r-s1::0-0-s1");
  });

  it("does not treat a single row or column, or blurred kernels, as a grid", () => {
    expect(buildBicubicGrid([0, 0.2, 0.4, 0.6, 0.8, 1].map((c) => row(0, c, 1e-6)))).toBeNull();
    const blurred = [0, 0.5, 1].flatMap((b) => [0, 0.5].map((c) => row(b, c, 1e-6, "s1", { blur: 1.2 })));
    expect(buildBicubicGrid(blurred)).toBeNull();
  });

  it("spreads cells over the scale logarithmically", () => {
    expect(heatStep(1e-8, 1e-8, 1e-2, 7)).toBe(0);
    expect(heatStep(1e-5, 1e-8, 1e-2, 7)).toBe(3);
    expect(heatStep(1e-2, 1e-8, 1e-2, 7)).toBe(6);
    expect(heatStep(5, 5, 5, 7)).toBe(0);
  });

  it("ranks kernels by mean error across Samples and keeps the worst case", () => {
    const ranking = rankKernelsAcrossSamples([
      { key: "a1", label: "A", metric: 1e-6 }, { key: "a2", label: "A", metric: 9e-6 },
      { key: "b1", label: "B", metric: 3e-6 }, { key: "b2", label: "B", metric: 4e-6 },
    ]);
    expect(ranking.map((item) => item.label)).toEqual(["B", "A"]);
    expect(ranking[1]).toMatchObject({ worst: 9e-6, count: 2, key: "a1" });
  });
});
