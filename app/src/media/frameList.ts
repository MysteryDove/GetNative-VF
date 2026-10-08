export type ParsedFrameList = {
  /** Valid frame indices, de-duplicated, in the order they were written. */
  frames: number[];
  /** Indices at or past the end of the stream. */
  outOfRange: number[];
  /** Tokens that are not a non-negative integer. */
  invalid: string[];
};

/**
 * Parses a pasted list of frame numbers. Accepts whatever separates them in
 * practice: spaces (vspreview scening), commas, semicolons, newlines, and the
 * brackets of a Python list.
 */
export function parseFrameList(text: string, totalFrames: number | null): ParsedFrameList {
  const frames: number[] = [];
  const outOfRange: number[] = [];
  const invalid: string[] = [];
  const seen = new Set<number>();
  for (const token of text.split(/[\s,;[\]()]+/)) {
    if (!token) continue;
    if (!/^\d+$/.test(token) || !Number.isSafeInteger(Number(token))) {
      invalid.push(token);
      continue;
    }
    const frame = Number(token);
    if (seen.has(frame)) continue;
    seen.add(frame);
    if (totalFrames != null && frame >= totalFrames) outOfRange.push(frame);
    else frames.push(frame);
  }
  return { frames, outOfRange, invalid };
}
