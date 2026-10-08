import { describe, expect, it } from "vitest";
import { parseFrameList } from "./frameList";

describe("parseFrameList", () => {
  it("reads a space-separated vspreview scening list", () => {
    expect(parseFrameList("953 1780 2031 2337 5105", 34286)).toEqual({
      frames: [953, 1780, 2031, 2337, 5105],
      outOfRange: [],
      invalid: [],
    });
  });

  it("accepts commas, newlines and list brackets, dropping repeats", () => {
    expect(parseFrameList("[12, 7,\n 12; 0]", null).frames).toEqual([12, 7, 0]);
  });

  it("separates out-of-range and malformed entries", () => {
    expect(parseFrameList("5 100 abc -3 1.5 99", 100)).toEqual({
      frames: [5, 99],
      outOfRange: [100],
      invalid: ["abc", "-3", "1.5"],
    });
  });

  it("returns nothing for blank input", () => {
    expect(parseFrameList("  \n ", 10)).toEqual({ frames: [], outOfRange: [], invalid: [] });
  });
});
