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
import { addBicubicGridToScanList, clearScanList, defaultKernelDraft } from "./kernelDraft";
import { heightRunConfig } from "./runGroupPlan";

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
