import { describe, expect, it } from "vitest";
import {
  finalSourceIdFor,
  mergeProbedSource,
  sourceFromProbe,
  stillSampleForSource,
} from "./importSources";
import { emptyProjectState } from "../project/normalize";
import type { MediaProbeResult } from "./service";

const stillProbe: MediaProbeResult = {
  path: "/tmp/frame.png",
  file_name: "frame.png",
  kind: "still",
  state: "ready",
  fingerprint: "fp-1",
  size_bytes: 100,
  width: 1920,
  height: 1080,
  duration_seconds: null,
  decoder: "image-rs/Png",
  video_streams: [],
  selected_stream_index: null,
  diagnostic: null,
};

describe("importSources", () => {
  it("dedups probed sources by path+fingerprint and resolves the final id", () => {
    const existing = sourceFromProbe("src_existing", stillProbe);
    const byId = { src_existing: existing };
    // A re-import of the same file drops the provisional record.
    const merged = mergeProbedSource(byId, "src_prov", stillProbe);
    expect(merged.src_prov).toBeUndefined();
    expect(merged.src_existing).toBeDefined();
    expect(finalSourceIdFor(merged, "src_prov", stillProbe)).toBe("src_existing");
    // A different file keeps the provisional id.
    const other = { ...stillProbe, path: "/tmp/other.png", fingerprint: "fp-2" };
    const merged2 = mergeProbedSource(byId, "src_prov", other);
    expect(finalSourceIdFor(merged2, "src_prov", other)).toBe("src_prov");
  });

  it("creates one included still Sample per ready still Source, never duplicates", () => {
    const state = emptyProjectState({ id: "p1" });
    state.sourcesById.src_1 = sourceFromProbe("src_1", stillProbe);
    const sample = stillSampleForSource(state, "src_1", () => "abc");
    expect(sample).not.toBeNull();
    expect(sample?.included).toBe(true);
    expect(sample?.frameIndex).toBeNull();
    expect(sample?.sourceFingerprint).toBe("fp-1");
    expect(sample?.id).toBe("smp_abc");

    state.samplesById[sample!.id] = sample!;
    expect(stillSampleForSource(state, "src_1")).toBeNull();

    // Non-ready or non-still sources never produce Samples.
    state.sourcesById.src_2 = sourceFromProbe("src_2", {
      ...stillProbe,
      path: "/tmp/clip.mkv",
      kind: "video",
    });
    expect(stillSampleForSource(state, "src_2")).toBeNull();
    state.sourcesById.src_3 = sourceFromProbe("src_3", {
      ...stillProbe,
      path: "/tmp/broken.png",
      state: "error",
    });
    expect(stillSampleForSource(state, "src_3")).toBeNull();
  });
});
