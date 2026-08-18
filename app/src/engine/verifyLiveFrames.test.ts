import { afterEach, describe, expect, it, vi } from "vitest";
import { VERIFY_LIVE_FLUSH_MS, VerifyLiveFrameBuffer } from "./verifyLiveFrames";

afterEach(() => vi.useRealTimers());

describe("VerifyLiveFrameBuffer", () => {
  it("retains batches as chunks and invalidates at most once per 200ms window", () => {
    vi.useFakeTimers();
    const invalidate = vi.fn();
    const buffer = new VerifyLiveFrameBuffer(invalidate);

    for (let seq = 0; seq < 3; seq += 1) {
      buffer.append("run-1", [{ seq, frameIndex: seq, error: seq }]);
    }
    expect(invalidate).not.toHaveBeenCalled();
    vi.advanceTimersByTime(VERIFY_LIVE_FLUSH_MS - 1);
    expect(invalidate).not.toHaveBeenCalled();
    vi.advanceTimersByTime(1);
    expect(invalidate).toHaveBeenCalledTimes(1);
    expect(buffer.snapshotAll()["run-1"]?.map((frame) => frame.seq)).toEqual([0, 1, 2]);

    buffer.append("run-1", [{ seq: 3, frameIndex: 3, error: 3 }]);
    vi.advanceTimersByTime(VERIFY_LIVE_FLUSH_MS);
    expect(invalidate).toHaveBeenCalledTimes(2);
    buffer.dispose();
  });

  it("drops terminal run chunks immediately and cancels the pending refresh", () => {
    vi.useFakeTimers();
    const invalidate = vi.fn();
    const buffer = new VerifyLiveFrameBuffer(invalidate);
    buffer.append("run-1", [{ seq: 0, frameIndex: 0, error: 1 }]);

    buffer.clear("run-1");

    expect(buffer.snapshotAll()["run-1"]).toBeUndefined();
    expect(invalidate).toHaveBeenCalledTimes(1);
    vi.advanceTimersByTime(VERIFY_LIVE_FLUSH_MS);
    expect(invalidate).toHaveBeenCalledTimes(1);
  });
});
