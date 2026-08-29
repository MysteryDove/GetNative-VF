import { describe, expect, it } from "vitest";
import {
  baseForMode,
  geometryCandidate,
  geometryForSource,
  migrateGeometrySnapshot,
  minimumBaseForParity,
  resolveGeometryValues,
  srcFromScanSelection,
} from "./geometry";

describe("fractional geometry semantics", () => {
  it("uses integer mode as src == canvas with zero offset", () => {
    const geometry = resolveGeometryValues({
      sourceWidth: 1920,
      sourceHeight: 1080,
      srcWidth: 1488,
      srcHeight: 837,
      baseWidth: null,
      baseHeight: null,
    });
    expect(geometry.canvasWidth).toBe(1488);
    expect(geometry.canvasHeight).toBe(837);
    expect(geometry.srcWidth).toBe(1488);
    expect(geometry.srcHeight).toBe(837);
    expect(geometry.srcLeft).toBe(0);
    expect(geometry.srcTop).toBe(0);
  });

  it("rounds fractional src values when both bases are integer mode", () => {
    const geometry = resolveGeometryValues({
      sourceWidth: 1920,
      sourceHeight: 1080,
      srcWidth: 1488.5,
      srcHeight: 837.25,
      baseWidth: null,
      baseHeight: null,
    });
    expect(geometry.srcWidth).toBe(1488);
    expect(geometry.srcHeight).toBe(837);
    expect(geometry.srcLeft).toBe(0);
    expect(geometry.srcTop).toBe(0);
  });

  it("preserves fractional height and computes the centered offset", () => {
    const geometry = resolveGeometryValues({
      sourceWidth: 1920,
      sourceHeight: 1080,
      srcWidth: 1488,
      srcHeight: 837,
      baseHeight: 1000,
      baseWidth: null,
    });
    expect(geometry.canvasHeight).toBe(838);
    expect(geometry.srcTop).toBe(0.5);
    expect(geometry.baseHeight).toBe(1000);
    expect(geometry.canvasWidth).toBe(1488);
    expect(geometry.srcLeft).toBe(0);
  });

  it("allows a parity base larger than the source dimensions", () => {
    const geometry = resolveGeometryValues({
      sourceWidth: 1920,
      sourceHeight: 1080,
      srcWidth: 1920,
      srcHeight: 843.8,
      baseWidth: null,
      baseHeight: 1081,
    });
    expect(geometry.baseHeight).toBe(1081);
    expect(geometry.canvasHeight).toBe(845);
    expect(geometry.srcHeight).toBe(843.8);
    expect(geometry.srcTop).toBeCloseTo(0.6);
  });

  it("keeps width and height base parity independent", () => {
    const geometry = resolveGeometryValues({
      sourceWidth: 1920,
      sourceHeight: 1080,
      srcWidth: 1488.5,
      srcHeight: 837.25,
      baseWidth: 1490,
      baseHeight: 1001,
    });
    expect(geometry.canvasWidth).toBe(1490);
    expect(geometry.canvasHeight).toBe(839);
    expect(geometry.srcLeft).toBe(0.75);
    expect(geometry.srcTop).toBe(0.875);
  });

  it("generates the smallest integer base for each parity", () => {
    expect(minimumBaseForParity(837, "even")).toBe(838);
    expect(minimumBaseForParity(837, "odd")).toBe(837);
    expect(minimumBaseForParity(837.1, "odd")).toBe(839);
    expect(baseForMode(837, "integer")).toBeNull();
  });

  it("uses src rather than canvas for candidates", () => {
    const geometry = resolveGeometryValues({
      sourceWidth: 1920,
      sourceHeight: 1080,
      srcWidth: 1488.5,
      srcHeight: 837,
      baseWidth: 1490,
      baseHeight: 1000,
    });
    expect(geometryCandidate(geometry, "w_only")).toBe(1488.5);
    expect(geometryCandidate(geometry, "h_only")).toBe(837);
  });

  it("maps selections to the active axis and derives h_plus_w width", () => {
    expect(srcFromScanSelection({
      axisMode: "h_only", selected: 837, sourceWidth: 1920, sourceHeight: 1080,
    })).toEqual({ srcWidth: 1920, srcHeight: 837 });
    expect(srcFromScanSelection({
      axisMode: "w_only", selected: 1488.5, sourceWidth: 1920, sourceHeight: 1080,
    })).toEqual({ srcWidth: 1488.5, srcHeight: 1080 });
    expect(srcFromScanSelection({
      axisMode: "h_plus_w", selected: 810, sourceWidth: 1920, sourceHeight: 1080,
    })).toEqual({ srcWidth: 1440, srcHeight: 810 });
  });

  it("re-resolves a locked recipe for a different source shape", () => {
    const original = resolveGeometryValues({
      sourceWidth: 1920,
      sourceHeight: 1080,
      srcWidth: 1440,
      srcHeight: 810,
      baseWidth: 1441,
      baseHeight: 811,
    });
    const resized = geometryForSource(original, "h_plus_w", 1280, 720);
    expect(resized.srcHeight).toBe(810);
    expect(resized.srcWidth).toBe(1440);
    expect(resized.baseWidth).toBe(1441);
    expect(resized.baseHeight).toBe(811);
  });

  it("marks ambiguous old fractional geometry for review", () => {
    const migrated = migrateGeometrySnapshot({
      mode: "standard",
      activeWidth: 1488.5,
      activeHeight: 837,
      canvasWidth: 1489,
      canvasHeight: 837,
    });
    expect(migrated?.needsReview).toBe(true);
    const integer = migrateGeometrySnapshot({
      mode: "standard",
      canvasWidth: 1488,
      canvasHeight: 837,
      baseHeight: 720,
    });
    expect(integer?.needsReview).toBe(false);
    expect(integer?.srcWidth).toBe(1488);
    expect(integer?.baseHeight).toBeNull();
    expect(migrateGeometrySnapshot({
      mode: "pro", canvasWidth: 1490, canvasHeight: 838,
      srcLeft: 0.5, srcTop: 0.5, srcWidth: 1489, srcHeight: 837,
    })?.needsReview).toBe(true);
    expect(migrateGeometrySnapshot({
      canvas_width: 1488, canvas_height: 837,
    })?.srcHeight).toBe(837);
    const missingCanvas = migrateGeometrySnapshot({
      activeWidth: 1488.5, activeHeight: 837,
    });
    expect(missingCanvas?.needsReview).toBe(true);
    expect(missingCanvas?.srcWidth).toBe(1488.5);
  });
});
