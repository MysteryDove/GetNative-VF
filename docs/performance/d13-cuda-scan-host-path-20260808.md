# D1.3 CUDA Scan Host Path: Pack-Into-Pinned + Pipeline Depth 3

- Date: 2026-08-08.
- Branch: `main` (follow-up to D1.2; commits `47da36e`, `a3ca8ae`,
  `7e01b9b`, `4d3dd59`, `41b89e9`).
- Host: Linux x86-64, AMD Ryzen 9 5950X, NVIDIA RTX 5080 (sm_120, driver
  595.84), CUDA 13.3, Release build, GCC 15.2.
- Decision: **ADOPT** (worker stage telemetry; source-preserving budget
  resets; persistent packed batch with exact pre-pass; single-pass forward
  pack; pack directly into pinned staging; pipeline default depth 2 → 3).

## 1. Where the e2e time actually went

E2E probing of the real worker chain (301 unique candidates, 1080p,
h_plus_w, fresh plan grids, median of 3) with the offline benchmark's
per-stage telemetry exposed through the worker result showed the D1.2
candidate phase was host-pack-bound:

| Stage (lanczos8 h_plus_w, per scan) | Before | After |
| --- | ---: | ---: |
| host pack (sum over pipeline threads) | ~200 ms | 54–71 ms |
| plan upload device window | 37 ms | 11 ms |
| plan payload uploaded | 259 MB | 259 MB |
| candidate phase wall | 222 ms | **162 ms** |
| total incl. plan build | 343 ms | 270 ms |

bicubic h_plus_w: 52 → 44 ms candidate phase. h_only shapes improve
similarly (lanczos8 62 → 46 ms). GPU kernels are unchanged; baseline
bitwise-repeat gates and worker cuda-parity stay green.

Four compounding defects were removed:

1. **Fresh `PackedBatch` per chunk** — nine vectors grew from empty
   through the reallocation cascade and faulted fresh pages every chunk
   (~26 MB at lanczos8). Now persistent per slot, cleared with capacity
   retained, and pre-sized exactly (every packed range is a whole plan
   array, so a cheap pre-pass knows all sizes).
2. **Tap-major weight transform with 16x read amplification** — the old
   tap-outer loop re-read every row-major source cache line
   `forward_width` times. Now one row-outer pass reads the source once;
   the scattered tap-major writes land in ≤16 concurrently-hot output
   lines (~1 KiB, L1-resident). The `forward_left` gather folds into the
   same pass, and the transform writes directly into the batch image
   (no temporary vectors).
3. **Double host pass over the payload** — pack wrote vectors, then a
   staging loop memcpy'd them into pinned before the H2D copies. Pack now
   writes straight into the pinned staging image at precomputed offsets
   and the upload is nine H2D copies reading those regions. Descriptor
   bases are unchanged per-array element offsets → device bytes are
   bit-identical by construction (verified by the baseline gates).
4. **Budget resets evicted the source** — `reset_device_buffers` dropped
   the 2×8 MB source/transpose buffers together with the large plan
   buffers, forcing re-upload + re-transpose per batch under memory
   pressure (6x transpose time at lanczos8 in the throughput benchmark;
   source residency is generic across batches, so it now survives
   `reset_plan_buffers`). Frozen-shape benchmark (64 candidates, both,
   concurrency 16): bicubic 1188 → 1401 fps (+18%) from this alone.

## 2. Pipeline depth re-tuned

D1.2 picked depth 2 ("p4+ shows no further gain") — but that was while
host pack dominated. With pack cheap the knee moved (301 candidates,
1080p h_plus_w, median, depth 1/2/3/4):

| Kernel | p1 | p2 | p3 | p4 |
| --- | ---: | ---: | ---: | ---: |
| lanczos8 | 273 | 166 | **152** | 163 |
| bicubic | 59 | 42 | **41** | 41 |

Default depth is now 3 (`worker_count` remains the 1..8 override).
Chunk size stays 32: 64 was tested and regresses both depths (coarser
overlap granularity, worse tail).

## 3. Residual breakdown (post-change, lanczos8 h_plus_w per scan)

- GPU kernels ~203 ms summed over 3 slots (~70–90 ms wall) — now the
  binding constraint; inverse solve ≈ 60-65% of kernel time.
- Host pack 54–71 ms summed (~20–25 ms wall) — validate_axis_plan,
  append memcpys, and the forward transform remain.
- Plan upload window 11 ms (259 MB H2D).
- Plan build 118 ms per cold scan — next host-side lever (phase-reuse
  of tap weights: `sin` is ~18% of cold-scan CPU because cross-period
  distance values differ by 1 ulp and exact-double dedup misses).
- Dynamic-ring guard waste: bilinear (hbw=1 on the 15-deep ring) inverse
  is 53% slower per candidate than exact-fit bicubic (hbw=3) despite
  less work — a shallow tier would recover it.

## 4. What did NOT work / negative results

- Chunk size 64: lanczos8 p3 152 → 163 ms, bicubic 41 → 48 ms. Fill/
  drain and tile granularity dominate the per-chunk fixed-cost saving.
- Depth 4: regresses on host memory contention (163 vs 152 ms).
- `std::vector::resize_and_overwrite` would avoid the value-init pass in
  the reserve-then-write pattern but this libstdc++ (GCC 15.2) lacks it;
  the affected arrays are ~1/4 of the payload (~2 ms/scan), not worth a
  default-init allocator.

## 5. Follow-ups (recorded)

- ~~Plan-build phase-reuse quantization~~ — **LANDED** (`77a9042`):
  integer active_length geometries replay row weights across the position
  lattice period (rows/gcd rows). lanczos8 h_plus_w plan phase
  118 → 98 ms on the uniform 700..1000 grid (median period 270 caps the
  average at ~4x on weight evals); gcd-friendly native heights get
  10-270x per plan. Fractional active lengths fall back.
- Shallow-tier inverse ring (exact hbw=1, or 7-deep) once GPU is the
  wall after plan build improves.
- Per-plan packed-blob persistence (plan store v4) — the pinned layout
  work makes the packed form cheap to produce; storing it would skip
  build + pack on repeat sessions (revisits the E4 latency verdict).
