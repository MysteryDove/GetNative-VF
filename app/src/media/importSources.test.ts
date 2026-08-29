import { describe, expect, it, vi } from "vitest";
import {
  beginSourceMediaIndex,
  cancelSourceImport,
  finalSourceIdFor,
  indexedVideoProbe,
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

  it("keeps a video probing until one indexed default stream is committed", () => {
    const probing: MediaProbeResult = {
      ...stillProbe,
      path: "/tmp/clip.mkv",
      file_name: "clip.mkv",
      kind: "video",
      state: "probing",
      width: null,
      height: null,
      decoder: null,
    };
    const ready = indexedVideoProbe(probing, {
      kind: "video",
      state: "ready",
      fingerprint: "indexed-fp",
      size_bytes: 999,
      stream_index: 2,
      codec: "h264",
      width: 1280,
      height: 720,
      duration_seconds: 12.5,
      frame_count: 300,
      time_base_num: 1,
      time_base_den: 24000,
      decoder: "software",
      index_version: 1,
      rebuilt: true,
    });
    expect(probing.state).toBe("probing");
    expect(ready.state).toBe("ready");
    expect(ready.selected_stream_index).toBe(2);
    expect(ready.video_streams).toEqual([expect.objectContaining({
      index: 2, frame_count: 300, time_base_den: 24000,
    })]);
  });

  it("tracks every Source index so deleting the Source cancels the engine job", async () => {
    const cancel = vi.fn(async () => undefined);
    let finish!: (value: Parameters<typeof indexedVideoProbe>[1]) => void;
    const pending = new Promise<Parameters<typeof indexedVideoProbe>[1]>((resolve) => {
      finish = resolve;
    });
    const result = indexedVideoProbe({
      ...stillProbe,
      path: "/tmp/clip.mkv",
      kind: "video",
      state: "probing",
    }, {
      kind: "video", state: "ready", fingerprint: "fp", size_bytes: 1,
      stream_index: 0, codec: "h264", width: 16, height: 16,
      duration_seconds: 1, frame_count: 1, time_base_num: 1, time_base_den: 1,
      decoder: "software", index_version: 1, rebuilt: true,
    });
    const task = beginSourceMediaIndex(
      "src_video",
      { path: "/tmp/clip.mkv", fingerprint: "fp" },
      undefined,
      () => ({ requestId: "request", promise: pending, cancel }),
    );

    await cancelSourceImport("src_video");
    expect(cancel).toHaveBeenCalledOnce();
    finish({
      kind: "video", state: "ready", fingerprint: result.fingerprint, size_bytes: 1,
      stream_index: 0, codec: "h264", width: 16, height: 16,
      duration_seconds: 1, frame_count: 1, time_base_num: 1, time_base_den: 1,
      decoder: "software", index_version: 1, rebuilt: false,
    });
    await task.promise;
    await cancelSourceImport("src_video");
    expect(cancel).toHaveBeenCalledOnce();
  });
});
