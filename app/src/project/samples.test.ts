import { describe, expect, it } from "vitest";
import {
  hiddenResultSampleIds,
  resultSampleIsVisible,
  selectedAnalysisSamples,
} from "./samples";
import type { Sample } from "./types";

const sample = (id: string, order: number): Sample => ({
  id,
  sourceId: "src",
  sourceFingerprint: null,
  label: id,
  included: true,
  order,
  frameIndex: null,
  streamIndex: null,
  pts: null,
  bestEffortTimestamp: null,
  timeBaseNum: null,
  timeBaseDen: null,
  timestampSeconds: null,
  tags: [],
});

describe("selectedAnalysisSamples", () => {
  it("keeps all included samples when no one-shot selection is supplied", () => {
    const samples = [sample("a", 0), sample("b", 1)];
    expect(selectedAnalysisSamples(samples)).toBe(samples);
    expect(selectedAnalysisSamples(samples, [])).toBe(samples);
  });

  it("limits Analyze input to the Samples selected on the Samples page", () => {
    const samples = [sample("a", 0), sample("b", 1), sample("c", 2)];
    expect(selectedAnalysisSamples(samples, ["c", "a"]).map((item) => item.id)).toEqual(["a", "c"]);
  });

  it("hides excluded historical results unless the global switch is enabled", () => {
    const included = sample("a", 0);
    const excluded = { ...sample("b", 1), included: false };
    const samplesById = { a: included, b: excluded };
    expect([...hiddenResultSampleIds(samplesById, new Set(), false)]).toEqual(["b"]);
    expect([...hiddenResultSampleIds(samplesById, new Set(["a"]), true)]).toEqual(["a"]);
    expect(resultSampleIsVisible(samplesById, "b", false)).toBe(false);
    expect(resultSampleIsVisible(samplesById, "b", true)).toBe(true);
  });
});
