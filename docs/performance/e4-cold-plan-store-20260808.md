# E4 Cold Plan Store: Single-Flight LRU + GNPK Chunked Packs

- Date: 2026-08-08.
- Branch: `main` (phase E4 of `docs/dsmvc-port-strategy.md`).
- Host: Linux x86-64, AMD Ryzen 9 5950X (16C/32T), NVMe page cache,
  Release build, GCC 15.2.
- Decision at evaluation time: **ADOPT** the L1 single-flight LRU (default,
  zero cost); adopt the L2 pack store as opt-in because parallel batch build
  won on this host class. Current product policy supersedes that default: L2
  is enabled and stores packs beside `getnative-engine` unless
  `GETNATIVE_PLAN_CACHE_DIR` overrides the directory or
  `GETNATIVE_PLAN_CACHE=off` disables it.

## 1. What landed

1. **L1 single-flight LRU** (`engine/src/planner/axis_plan.cpp`): fixed
   admission replaced by a byte+entry dual-cap LRU with a build mutex
   that serializes miss paths (concurrent callers never duplicate a
   build — dsmvc SingleFlightLru semantics at call granularity). New
   `publish()` (store preheat) and `lookup_batch()` (read-only,
   never-build) APIs. Default-on, behavior-neutral except under pressure.
2. **L2 pack store** (`engine/src/planner/plan_store.cpp`,
   `plan_serialize.cpp`): GNPK v3 — one file per (candidate grid × build
   fingerprint), 64 plans per chunk, LZ4 blocks behind a sorted key-hash
   index with XXH64 content checksums, cooked structural pre-transform
   (uniform forward offsets dropped, per-row forward anchors, CSR
   delta-varint), header FNV checksum, corrupt-pack-always-degrades-to-
   rebuild semantics, mtime-LRU sweep at 1 GiB, atomic no-replace
   publish, write-behind writer thread in the worker. Vendored:
   `third_party/zstd` (1.5.7, kept for codec 0), `third_party/lz4`
   (1.10.0, default codec), `third_party/lz4/xxhash` (content checksums).
3. **Worker wiring** (`engine/src/cli/worker.cpp`): whole-grid parallel
   prefetch (reverse-order publish so early-consumed plans stay newest in
   the LRU) + per-round sparse top-up for LRU-evicted entries +
   telemetry `plan_store_hits` / `plan_store_fetch_ms`.

## 2. Exit criterion (hit rate): MET

`engine/bench/plan_store_probe.py`, 1,000-candidate grid
(1080 → 700..1079.62 step 0.38), two processes sharing a store dir:

| Kernel | Process A cold | Process B warm | Effective hit rate | Parity |
| --- | --- | --- | --- | --- |
| lanczos6 | builds 1000, plan_ms 107 | **builds 0**, store_hits 1091 | 79.7% → **100%** | exact |
| lanczos8 | builds 1000, plan_ms 148 | **builds 0**, store_hits 1306 | 69.8% → **100%** | exact |

Same-process rescans also rebuild nothing (the L1 LRU retains what fits;
the store re-serves the evicted tail). `plan_store_tests` covers
roundtrip identity (8 kernels × 3 borders × 4 shapes), fingerprint and
chunk-corruption gating with file deletion, no-replace publish, eviction
sweep, LRU victim selection, 1,000-plan retention, single-flight
instance identity; the worker protocol suite covers cross-process reuse
with exact-error parity (`store-cross-process-*`).

## 3. Latency (the honest part): store LOSES to rebuild on this host

| Kernel | Cold build | Warm fetch | Rescan fetch |
| --- | ---: | ---: | ---: |
| lanczos6 | 107 ms | 123 ms (1.15x) | 91 ms (0.85x) |
| lanczos8 | 148 ms | 198 ms (1.34x) | 160 ms (1.08x) |

A 1,000-plan batch builds in ~107-148 ms on 16 planner workers
(~0.14 ms/plan amortized). The best fetch pipeline achieved is
~0.12-0.20 ms/plan. The store's premise ("large-taps rebuilds are the
expensive case") does not survive contact with the parallel batch build
at 16 cores; the cliff the store fixes (30% of 143 ms ≈ 43 ms per
over-cap scan) is cheaper than the cure. The evaluation therefore recommended
opt-in operation; the current product requirement instead enables L2 by
default beside the executable. It still wins where the parallel build is weak
(few-core hosts) and for sparse single-plan patterns a batch rebuild cannot
serve cheaply. The L1 LRU adoption is unaffected and stays default-on.

### 3.1 Latency bugs found and fixed en route (measured)

The gap between the evaluation's ~0.4 ms/chunk estimate and the first
implementation (155 ms/chunk) was four compounding defects, each found
by profiling the fetch path directly:

1. **Whole-file header reads**: `load_pack_header` read the entire pack
   per sparse call (16 × 170-226 MB per scan). Fixed to header-only
   reads + lazy chunk fetch: 2488 → 799 ms warm plan phase.
2. **FNV-1a content checksum**: byte-at-a-time over ~36 MiB chunks on
   every decode. Switched to XXH64 (vendored): 1055 → 534 ms.
3. **Codec choice**: zstd-1 frames (~12 ms per 36 MiB chunk) → LZ4
   blocks (~3 ms), the escape hatch the evaluation pre-authorized;
   +40% pack size. Combined with (2).
4. **Little-endian key serialization**: lexicographic byte order
   scrambled numeric key order, scattering each 64-plan job round across
   2-4 pack chunks. Big-endian sign-transformed key bytes realigned
   rounds to single chunks; with parallel chunk decode, whole-grid
   prefetch, reverse-order publish, and per-round sparse top-up:
   534 → 123/198 ms.

Format is GNPK **v3** (v2 packs from the first day of development are
rejected and rebuilt; nothing older exists anywhere else).

## 4. What the evaluation got right and wrong

`docs/cold-plan-cache-evaluation.md` predicted: content-independent keys
(right, 100% reuse across images), chunked packs with sparse reads
(right, used by the top-up path), build-fingerprint gating (right), and
the fixed-admission cliff (right). It underestimated decode+validation
per plan (its 0.4 ms/chunk was decompress-only; real pipelines pay
io+codec+decode+validate+publish) and it could not know the four §3.1
defects. Its final framing stands: this is a **robustness feature, not
a throughput feature** — measured here down to the keep/reject split.

## 5. Follow-ups (recorded, not scheduled)

- Sparse single-plan access for mixed-grid verification reuse (the read
  path supports it; no consumer yet).
- Windows store path validation (the code is portable; only POSIX hosts
  were measured).
- Revisit default-on if builds move to low-core targets or if packs
  learn cross-grid plan dedup (content-addressed plan store).

## 6. v4 addendum (2026-08-08, P3-7 revisit)

A follow-up attempt to flip the latency verdict measured the fetch
pipeline stage by stage (`GETNATIVE_STORE_DEBUG_TIMING=1` on
`read_grid`; a cancel-isolated perf profile of the plan phase). Every
suspect was individually small: v3 varint/delta decode ~4%, the
`require_finite` validation passes ~2% (env-gated A/B), allocator
first-touch ~2% (worse with `mmap_threshold` raised via GLIBC_TUNABLES),
publish ~0.3%. The floor is the payload: ~255 MB per 1000 lanczos8
plans moves through memory 4-5 times (read ~25 ms, LZ4 ~30 ms, XXH64
~8 ms, AxisPlan materialization ~25-50 ms, fault overhead), landing
within ~1.3x of the 16-core parallel build regardless of encoding. The
period-replay planner speedup (`77a9042`, integer grids 148 → 98 ms per
1000 plans) moved the bar further against the store.

Landed anyway: GNPK **v4** stores transpose offsets/indices raw (one
bounds-checked pass replaces per-element varint/delta decode), fetch
−4%, pack +5-7%, −30 lines (`9a5dda4`). A zero-copy/span-backed AxisPlan
consuming blobs in place could reach ~1.3-1.6x over build for sinc
kernels only — below the bar for that refactor. **Measurement verdict:
not a throughput feature on this host class.** The later product decision to
enable it by default is about portable cross-process reuse. Poly-kernel grids
(bicubic/bilinear/spline) build in ~1 ms per 1000 plans, so no encoding
can ever win there; any future flip should also be filter-gated.
