import type { VerifyFrameEntry } from "./executeVerify";

export const VERIFY_LIVE_FLUSH_MS = 200;

type TimerHandle = ReturnType<typeof setTimeout>;

/**
 * Retains incoming worker batches as chunks and invalidates the UI at most
 * once per flush window. Flattening happens only when a rendered snapshot is
 * requested, rather than on every 32-frame worker batch.
 */
export class VerifyLiveFrameBuffer {
  private readonly chunksByRun = new Map<string, VerifyFrameEntry[][]>();
  private timer: TimerHandle | null = null;

  constructor(
    private readonly onInvalidate: () => void,
    private readonly schedule: (callback: () => void, delayMs: number) => TimerHandle = setTimeout,
    private readonly cancel: (handle: TimerHandle) => void = clearTimeout,
  ) {}

  append(runId: string, entries: VerifyFrameEntry[]): void {
    if (!entries.length) return;
    const chunks = this.chunksByRun.get(runId);
    if (chunks) chunks.push(entries);
    else this.chunksByRun.set(runId, [entries]);
    if (this.timer !== null) return;
    this.timer = this.schedule(() => {
      this.timer = null;
      this.onInvalidate();
    }, VERIFY_LIVE_FLUSH_MS);
  }

  snapshotAll(): Record<string, VerifyFrameEntry[]> {
    return Object.fromEntries(
      [...this.chunksByRun].map(([runId, chunks]) => [runId, chunks.flat()]),
    );
  }

  clear(runId: string): void {
    if (!this.chunksByRun.delete(runId)) return;
    if (this.timer !== null) {
      this.cancel(this.timer);
      this.timer = null;
    }
    this.onInvalidate();
  }

  dispose(): void {
    if (this.timer !== null) this.cancel(this.timer);
    this.timer = null;
    this.chunksByRun.clear();
  }
}
