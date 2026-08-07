# E1 CUDA Batch Scheduling: Session Worker Path and Source Residency

- Date: 2026-08-08.
- Branch: `engine/worker-protocol` (phase E1 of `docs/dsmvc-port-strategy.md`).
- Host: Linux x86-64 (32 logical CPUs), NVIDIA GeForce RTX 5080 (sm_120,
  driver 595.84), CUDA 13.3 toolkit, Release build, GCC 15.2.
- Decision: **ADOPT** (source residency + worker CUDA session path).

## 1. What changed

1. **Worker CUDA session path** (`engine/src/cli/worker.cpp`): the worker
   holds one session-lived `CudaAnalysisEngine` (context, embedded fatbin,
   execution slots, and slot-local caches are created once and reused
   across jobs). `backend:"cuda"` routes height analysis to it; `auto`
   keeps the CPU-oracle contract. Cancellation flows through the existing
   job stop token into `analyze_axis_batch_f32`.
2. **Session frame cache** (`worker.cpp`): frame assets are loaded once
   per worker session into never-resized buffers (8-entry LRU), so the
   `ConstImageView` pointer is stable across jobs. Bulk pixels still never
   cross JSON.
3. **CUDA source residency** (`cuda_backend.cpp`): each execution slot
   remembers the resident device source (`SourceIdentity` = host pointer +
   geometry + 16-sample content probe). On a hit, the pinned staging copy,
   the H2D upload, and the source transpose kernel are all skipped. The
   content probe prevents stale hits from recycled host addresses.
   `reset_device_buffers` invalidates the identity. New telemetry:
   `source_cache_hits` / `source_cache_misses`; `source_upload_bytes` is 0
   on hits.

## 2. Ground truth (before)

Frozen-baseline shape from `cuda-profile-20260802.md`:
1920x1080 → 1280x720, 64 candidates, `both`, concurrency 16.

| Metric | Documented (2026-08-02) | This host, pre-change |
| --- | --- | --- |
| frame_ms | 7.613 | 7.500 |
| FPS | 131.35 | 133.33 |
| candidates/s | 8,406 | 8,533 |
| source upload per wave | 8.29 MB | 8.29 MB (663.5 MB / 80 waves) |

## 3. Evidence (after), same shape

`getnative_cuda_throughput_benchmark --width 1920 --height 1080
--native-width 1280 --native-height 720 --axes both --candidates 64
--concurrency 16 --samples 10`, three runs:

| Run | frame_ms | FPS | candidates/s | source_upload_bytes | source_cache_hits | checksum |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 6.4218 | 155.72 | 9,966 | 0 | 160 | 0.3454266449047882 |
| 2 | 6.6558 | 150.24 | 9,616 | 0 | 160 | 0.3454266449047882 |
| 3 | 6.4137 | 155.92 | 9,979 | 0 | 160 | 0.3454266449047882 |

- frame_ms 7.500 → 6.414 median (**-14.5%**); candidates/s 8,533 → 9,979
  (**+16.9%**). Checksum deterministic across runs.
- All 160 waves skipped the source upload entirely (one upload total,
  absorbed by warmup). The gain comes from removing one 8.29 MB H2D copy
  plus the transpose launch per wave.

## 4. Worker-session evidence

Integration test (`engine/tests/worker_protocol_test.py`, session 3):

- **cuda-parity**: CPU vs CUDA errors on the same frame/candidates agree
  within 1e-6 (relative).
- **cuda-source-residency**: first CUDA job reports
  `cuda_source_upload_bytes = 307200` (320x240 frame); second job with the
  same frame asset reports `0` and `cuda_source_cache_hits >= 1`. The
  session plan cache also reports warm hits across jobs
  (`plan_cache_hits`, `cuda_plan_cache_hits`).
- First-job latency includes one-time engine init (~335 ms on this host);
  identical follow-up jobs complete in ~0.35 ms.

## 5. Test-harness postmortem (recorded for future lane owners)

During bring-up, the worker integration test intermittently "hung" in CUDA
initialization. Investigation (env-gated init tracing) proved the engine
init completed every time; the real defect was in the Python harness:
`select()` mixed with buffered `readline()` loses events when several
JSONL events arrive in one OS chunk. Fixed by binary stdout framing with
an internal byte buffer. No engine defect existed; the trace scaffolding
was removed. 8/8 consecutive full-suite runs pass after the fix.

## 6. Remaining E1 gaps (deliberate)

- Plan uploads still serialize before the first tile of a batch (the
  dsmvc `plan_stream` + event-handoff overlap is a candidate when
  unique-candidate scans show plan upload share > 5% of gpu_total).
- Verification (frame streaming) is out of scope here; it is phase E2.
- The worker capabilities envelope now reports CUDA
  `analysis_command_available=true` when the device is usable; Metal
  remains false until the Metal lane wires its worker path.

## 7. Addendum (2026-08-08): same-session interleaved A/B correction

Re-verification on the merge into main used an interleaved A/B against
the pre-change commit (`afce7a3`, detached worktree, same toolchain,
alternating PRE/POST runs to share GPU clock state). Same shape as §3.

| Run | frame_ms PRE | frame_ms POST | c/s PRE | c/s POST | upload PRE | upload POST |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 7.955 | 7.675 | 8,045 | 8,339 | 1,327,104,000 | 0 |
| 2 | 7.821 | 7.454 | 8,183 | 8,587 | 1,327,104,000 | 0 |
| 3 | 7.681 | 7.551 | 8,332 | 8,476 | 1,327,104,000 | 0 |
| median | 7.821 | 7.551 | 8,183 | 8,476 | | |

- Same-session delta: frame_ms **-3.5%**, candidates/s **+3.6%** — not the
  -14.5%/+16.9% recorded in §3. The §3 PRE number (7.500) was measured in
  a different moment than the POST runs; GPU clock residency between
  sessions (observed ~2,000 vs up to 3,105 MHz, clocks not lockable
  without root) inflated the cross-session delta. The interleaved A/B is
  the reliable estimate for this shape.
- The structural win is unaffected: 1.33 GB of H2D uploads plus 160
  transpose launches eliminated per run, checksum bit-identical across
  PRE/POST (`0.34542664490478819`), `source_cache_hits` 160/160.
- Telemetry caveat discovered during re-verification: on cache hits the
  `source_upload_ms` / `source_transpose_ms` event-pair timers measure
  stream backlog between the bracketing events, not skipped work (bytes
  counters are the authoritative residency signal). Only interpret the
  `*_ms` fields when the corresponding byte counter is non-zero.
