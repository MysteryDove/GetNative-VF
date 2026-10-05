import { describe, expect, it } from "vitest";
import { filterAccepts, pruneFilter, toggleFilterValue } from "./multiFilter";

describe("multi-select filters", () => {
  it("accepts everything while nothing is selected", () => {
    expect(filterAccepts([], "a")).toBe(true);
    expect(filterAccepts(["a", "b"], "b")).toBe(true);
    expect(filterAccepts(["a", "b"], "c")).toBe(false);
  });

  it("toggles one value and returns to All when the last one is cleared", () => {
    expect(toggleFilterValue([], "a")).toEqual(["a"]);
    expect(toggleFilterValue(["a"], "b")).toEqual(["a", "b"]);
    expect(toggleFilterValue(["a", "b"], "a")).toEqual(["b"]);
    expect(toggleFilterValue(["b"], "b")).toEqual([]);
  });

  it("prunes vanished values and keeps the array identity when unchanged", () => {
    const selected = ["a", "b"];
    expect(pruneFilter(selected, ["a", "b", "c"])).toBe(selected);
    expect(pruneFilter(selected, ["b"])).toEqual(["b"]);
    expect(pruneFilter(selected, [])).toEqual([]);
  });
});
