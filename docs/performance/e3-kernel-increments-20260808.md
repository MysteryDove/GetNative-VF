# E3 Kernel Increments: Metric SIMD, Large-Taps CUDA Study

- Date: 2026-08-08.
- Branch: `main` (phase E3 of `docs/dsmvc-port-strategy.md`, D3 items).
- Host: Linux x86-64, AMD Ryzen 9 5950X (16C/32T), NVIDIA RTX 5080
  (sm_120, driver 595.84), CUDA 13.3, Release build, GCC 15.2.

## 1. SIMD metric accumulator: ADOPTED (fused, bit-exact)

D3's premise ("38% of the Lanczos6 CPU profile is scalar
`MetricAccumulator::add`") is already stale on the primary path: a flat
perf profile of the current verify workload (3,000 frames, bicubic,
16 workers) shows `inverse_columns_avx2_f32` 58.85% and the **already
fused** `vertical_reconstruction_norm1_avx2_f32` 24.02% — the scalar
accumulator is outside the top functions there. The remaining
scalar-accumulate consumer was `add_absolute_difference_row`, hot in
the `w_only` analysis path, the `vertical_first` 2D branch, and the
public `thresholded_p_norm`.

Change: new fused row kernel `absolute_difference_norm1_{sse2,avx2,
avx512}_f32` in the analysis dispatch, mirroring the existing vertical
norm-1 kernels: vector absolute difference, threshold AND-mask, then
strictly ordered per-lane double adds where masked lanes contribute an
exact `+0.0` — so the result is **bit-identical** to the scalar
raster-order accumulator (verified by the new
`test_absolute_difference_norm1_fusion` unit test with threshold
boundary values, and by end-to-end parity below).

A/B (1,000-frame verify, 1920x1080 → 810, bicubic, 16 workers,
pre/post binaries interleaved):

| Shape | Baseline | Fused | Delta | Parity |
| --- | ---: | ---: | ---: | --- |
| w_only | 286.0 fps | 302.9 fps | **+5.9%** | exact (1000/1000 frames bit-identical) |
| h_only (control) | 1,090.7 fps | 1,100.8 fps | +0.9% (noise) | — |

## 2. Large-taps CUDA family study: per-kernel rewrite REJECTED

Setup: `--filter` added to `getnative_cuda_throughput_benchmark`;
frozen shape 1920x1080 → 1280x720, 64 candidates, `both`,
concurrency 16, 8 samples, RTX 5080.

| Filter | FPS | candidates/s | inverse H+V share of kernel_ms |
| --- | ---: | ---: | ---: |
| bicubic | 162.1 | 10,376 | 72% |
| lanczos3 | 134.2 | 8,589 | 70% |
| bilinear | 93.3 | 5,972 | 76% (runtime-generic path) |
| spline64 | 42.4 | 2,716 | 76% |
| lanczos8 | 23.2 | 1,487 | **78%** |

Nsight Compute attribution (lanczos8, 16 candidates, concurrency 4):

| Kernel | Duration | SM throughput | Occupancy | Grid |
| --- | ---: | ---: | ---: | --- |
| `cuda_inverse_horizontal` | 9.83 ms | **9.1%** | 13.6% | (17,16,1)×(64,1,1) |
| `cuda_inverse_vertical` | 2.68 ms | 22.0% | 14.9% | (20,16,1)×(64,1,1) |
| `cuda_both_fused_metric` | 0.99 ms | 57.0% | 98.9% | (1,16,1064)×(256) |
| `cuda_transpose_source` | 0.01 ms | 24.1% | 89.7% | — |

The inverse solve kernels launch (tiles × candidates) blocks of 64
threads: 272-320 blocks ≈ **0.16-0.32 waves per SM** — the GPU is
parallelism-starved, not compute- or bandwidth-bound (DRAM ≤ 10%,
registers 40-48/thread, no local memory). This is exactly the ceiling
dsmvc measured (`~2.1% occupancy` at R32T32) and exactly what its
batch-8 merged-solve experiment proved recoverable (4.32x kernel-level).

Keep/reject: **reject per-kernel micro-optimization** for wide taps.
The launch policy is already frozen-optimal (autotune contract,
2026-08-03: no policy beats baseline past noise gates on any family).
The recoverable headroom is candidate-batched merged solves — the D1.2
follow-up (batch candidates into merged inverse launches on top of the
E1 residency work), where the merged-solve analog estimates ≈2-4x on
wide-tap families. Bilinear's generic-path gap vs bicubic's ring
specialization (93 vs 162 fps at equal work) is a second, smaller
candidate for a B2 ring specialization, worth ~1.5x on that family only
— deferred behind D1.2 as well.

## 3. Plan cache policy A/B: closed by E4

The D3 cache-policy item was executed in E4: fixed admission →
single-flight LRU adopted (docs/performance/e4-cold-plan-store-20260808.md
§2: 1,000-plan scans retain 100% under the raised-entry policy, and
eviction correctness is unit-tested).

## 4. CPU packed plans (dsmvc PackedCpuPlan): assessed, deferred

A full formal-matrix A/B needs the packed-plan port; it is deliberately
**not** started here. Rationale: the CPU analysis wall is
`inverse_columns_avx2_f32` (58.85%), a banded-solve kernel that already
has per-shape dispatch (`support3_b5_f6` specialization) and row-major
L2 blocking; dsmvc's packed layout attacks the same gather it does, but
the expected ceiling from a data-layout change (~1.2-1.5x on one kernel)
is smaller than the scheduling multipliers still on the table (D1.2
CUDA merged solves ≈2-4x wide-tap; CPU verification already scales with
workers). Ordered last of the D3 increments by design; revisit after
D1.2 if CPU single-thread throughput becomes the product constraint.
