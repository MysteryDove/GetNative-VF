# E2 Verification Pipeline: Worker Verify Mode, Frame-Parallel, mmap Assets

- Date: 2026-08-08.
- Branch: `main` (phase E2 of `docs/dsmvc-port-strategy.md`).
- Host: Linux x86-64, AMD Ryzen 9 5950X (16C/32T), NVIDIA RTX 5080 unused
  here (verify is CPU-only in protocol v1.1), Release build, GCC 15.2.
- Decision: **ADOPT** (streaming verify over the worker protocol, mmap
  frame assets, default 16 analysis workers).

## 1. What changed

1. **Protocol v1.1 verify streaming** (`engine/src/cli/worker.cpp`):
   `verify_begin` / `verify_frame` / `verify_end` commands implement the
   one-locked-recipe × many-frames workload. The reader thread appends
   frame items while the executor's analysis workers consume them, so
   producer decode overlaps analysis; per-frame results stream back in
   batched `progress` events; per-frame failures degrade to
   warning + null (mixed-success semantics for GUI-5); cooperative cancel
   preserves streamed results; the declared `total` is integrity-checked.
   Contract: `docs/worker-protocol-v1.md` §2.6.
2. **mmap single-use frame access** (POSIX): analysis consumes the
   producer's page-cache pages directly instead of `read(2)`-copying
   every frame into a user buffer. Producers publish atomically
   (tmp+rename), as the Tauri media layer already does. Windows keeps the
   buffered path until a `MapViewOfFile` branch lands.
3. **Frame-parallel analysis workers** (default `min(16, hardware)`),
   one `CpuWorkspace` per worker — the fixed-recipe prototype pattern
   (`fixed_recipe_benchmark.cpp`) productized behind the protocol.

## 2. Shape

`engine/bench/verify_episode_probe.py`: 1,000-frame streams over a ring
of 8 predecoded 1920x1080 f32le assets (the prototype's shape),
1920x1080 → 810, `h_only`, p=1 metric. Serial (`worker_count=1`) versus
default (16 workers). Protocol wall equals engine stream time within
0.05 ms everywhere — JSONL overhead is invisible at episode scale.

## 3. Results, read() asset path (first implementation)

| Kernel | workers=1 fps | workers=16 fps | Speedup |
| --- | ---: | ---: | ---: |
| bilinear | 356.9 | 701.9 | 1.97x |
| bicubic | 307.6 | 689.1 | 2.24x |
| lanczos6 | 187.8 | 675.4 | 3.60x |

Per-frame `frame_load_ms` degraded 11x from serial to 16 workers
(0.75 → 8.3 ms/frame) and accumulated `frame_analyze_ms` 7x — the
page-cache copy is DRAM-bandwidth contention, not compute. The M4 Max
prototype reached 2,056 fps median partly because its frames were
memory-resident and its memory system has ~5-10x the bandwidth headroom.

## 4. Results, mmap asset path (adopted)

| Kernel | workers=1 fps | workers=16 fps | Speedup vs read()-16 |
| --- | ---: | ---: | ---: |
| bilinear | 290.0 | 1,118.2 | +59% |
| bicubic | 227.7 | 1,092.1 | +58% |
| lanczos6 | 173.5 | 1,014.9 | +50% |

Worker-count sweep (bicubic, mmap): 4 → 780 fps, 8 → 1,005, 12 → 1,037,
16 → 1,110, 20 → 1,122, 24 → 1,139. The knee is at 8-16; SMT tails past
16 add < 3%. Default stays `min(16, hardware)`.

Serial regression (307.6 → 227.7 fps, analyze 2.49 → 3.52 ms/frame,
deterministic across runs): mapped page-cache pages are 4K-granular
(2,048 TLB entries per 1080p frame, rebuilt per mapping), while the
private buffer is THP-backed anonymous memory. At 16 workers the
bandwidth win dominates decisively; the serial case is a tuning/debug
configuration, so the mapping path is unconditional (no hidden
behavioral switch).

Episode projection at the adopted operating point (bicubic, 1,092 fps):
a 24-minute episode (~34,500 frames) is **~31.6 s analysis-only** over
the worker protocol, decode overlapped. The prototype's 16.8 s (M4 Max,
2,056 fps) does not transfer to this host — the difference is DRAM
bandwidth, not scheduling.

## 5. Correctness evidence

`engine/tests/worker_protocol_test.py` session 4 (all green on CPU and
CUDA builds):

- **verify-parity / verify-h-plus-w**: per-frame errors through verify
  equal the height-mode errors for the same frame/candidate/axis mode
  (|Δ| ≤ 1e-12) at both 320x240 and (via the probe) 1920x1080 shapes.
- **verify-plan-cache-hit**: a verify job whose recipe was scanned
  earlier in the session reports `plan_cache_hits=1`,
  `plan_build_count=0`.
- **verify-mixed-success**: one missing asset → one warning with its
  `seq`, `error: null` for that seq, job completes 2/3.
- **verify-total-mismatch**, **verify-frame-gap**,
  **verify-frame-unknown-job**, **verify-geometry-mismatch**,
  **verify-end-closed-stream**: stream-integrity errors leave the worker
  usable.
- **verify-cancel-partial**: cancel mid-stream; streamed results equal
  the cancelled payload's `frames_completed + frames_failed`.
- **verify-cuda-unsupported**: CUDA verify fails loudly, not silently.

## 6. Deliberate gaps (follow-ups)

- **CUDA frame streaming** (`backend: "cuda"` for verify): needs the
  D1-style device-resident pipeline per source; rejected with
  `unsupported` today.
- **Windows MapViewOfFile** branch for the mmap win.
- **Engine-side decode**: decode still lives in the Tauri media layer;
  the protocol's streaming shape makes the overlap work either way.
- **GUI-5 wiring** (app transport for the three commands, resume from
  streamed results): the UI lane consumes §2.6 of the protocol doc.
