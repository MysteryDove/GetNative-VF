# CUDA Profile Evidence - 2026-08-02

## Scope

This report covers the rebuilt Windows CUDA Driver-API backend. It does not
reuse the removed CUDA/Vulkan batch implementation or its generated artifacts.
The measured public variant is `cpp-generic`; compiler PTX is only a fallback,
and `cpp-specialized`, `architecture-specific`, and `inline-ptx` remain
fail-closed.

Verification host:

- GPU: NVIDIA GeForce RTX 5080, compute capability 12.0
- Profiler: Nsight Compute 2026.2.1
- Profiler access: all-user performance counters, successful 41/42-pass replay
- Build: MSVC 19.44, Release, CUDA target `sm_120 + compute_120`
- Workload: 1920x1080 source, 1280x720 native canvas, 64 Lanczos-3 candidates,
  crop 8, threshold 0, p=1, 3 warmups, 21 samples

These counters describe the native SM120 cubin. The subsequent portability
stage packages native SM75 through SM121 code plus compute_75/compute_121 PTX;
the driver still selects the same native SM120 code on this verification host.
Other architectures require their own real-device performance profiles.

The `.ncu-rep` files were kept outside the repository under
`C:\tmp\getnative-ncu`; generated CUDA/Vulkan artifacts are not source inputs.

## Final Throughput

Each entry is the median of five complete benchmark processes. Frame time is
normalized per concurrent analysis call.

| Axes | Concurrency | Frame ms | p95 ms | FPS | Candidates/s | Tiles/frame | Launches/frame |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| vertical | 1 | 4.025 | 4.179 | 248.43 | 15,899.83 | 1 | 3 |
| vertical | 16 | 3.667 | 3.755 | 272.72 | 17,454.15 | 1 | 3 |
| horizontal | 1 | 5.028 | 5.408 | 198.89 | 12,728.97 | 1 | 3 |
| horizontal | 16 | 4.116 | 4.257 | 242.96 | 15,549.38 | 1 | 3 |
| both | 1 | 9.676 | 9.962 | 103.35 | 6,614.30 | 1 | 5 |
| both | 16 | 7.613 | 7.841 | 131.35 | 8,406.47 | 1 | 5 |

Peak per-slot working sets are 352.59 MiB vertical, 366.51 MiB horizontal,
and 598.61 MiB combined. Slot wait remains below 0.52 ms accumulated across a
21-sample, concurrency-16 process; the concurrency limit is GPU throughput,
not the execution-slot mutex.

## Fatbin Resource Audit

`cuobjdump --dump-resource-usage` against the staged SM120 fatbin loaded by the
profiled benchmark reports no stack frame or local-memory allocation in any
kernel:

| Kernel | Registers | Static shared bytes | Stack bytes | Local bytes |
| --- | ---: | ---: | ---: | ---: |
| `getnative_cuda_transpose_source` | 30 | 5,248 | 0 | 0 |
| `getnative_cuda_inverse_horizontal` | 48 | 1,024 | 0 | 0 |
| `getnative_cuda_horizontal_fused` | 40 | 1,024 | 0 | 0 |
| `getnative_cuda_inverse_vertical` | 40 | 1,024 | 0 | 0 |
| `getnative_cuda_inverse_vertical_pair` | 48 | 1,024 | 0 | 0 |
| `getnative_cuda_forward_intermediate` | 40 | 1,024 | 0 | 0 |
| `getnative_cuda_both_fused_metric` | 40 | 3,072 | 0 | 0 |
| `getnative_cuda_metric_partials` | 40 | 1,024 | 0 | 0 |
| `getnative_cuda_metric_finalize` | 25 | 1,024 | 0 | 0 |

The audited output is `generated/cuda/getnative_cuda_staged.fatbin` in the
CUDA build tree. A separately named baseline fatbin in that build directory is
an earlier build artifact and is not the module loaded by the final benchmark.
The multi-architecture build now applies the same zero-stack/zero-local gate to
every native cubin through `getnative_cuda_artifact_inventory`.

## Retained Changes

### Coalesced plans and combined-axis dataflow

- Shared-memory local transpose in horizontal inverse reduced L2 sectors from
  60.54M to 31.54M, close to the 30.87M ideal, and improved the normal
  benchmark by about 14.5%.
- Tap-major forward weights reduced forward-plan sectors from 69.90M to 21.86M,
  equal to the ideal sector count, with 40 registers and no spill.
- The combined final pass stages a vertical row span and fuses horizontal
  reconstruction, thresholding, and metric reduction. Three-way staging plus
  two-way reconstruction ILP reduced profiler duration from 295.04 us to
  206.66 us, registers from 42 to 40, and long-scoreboard share from 37.23% to
  24.02% while achieved occupancy rose from 81.68% to 97.18%.

### Fixed-width F6 reconstruction

The common F6 path issues independent loads before its FMA chain while the
runtime-width fallback remains available.

- Horizontal kernel A/B: median frame time 5.272 -> 5.033 ms (-4.53%).
- Horizontal counter: 3.13 -> 3.03 ms, 135.28M -> 121.81M instructions,
  10.80M -> 10.35M L2 sectors, 40 registers, no spill.
- Vertical metric A/B: median frame time 4.485 -> 4.311 ms (-3.88%).
- Vertical metric counter: 218.88 -> 184.45 us (-15.73%), 74.50M -> 62.89M
  instructions, 40 registers, about 93% achieved occupancy, no spill.

### Paired vertical solve

The vertical-only entry point advances two independent, separately coalesced
column groups per thread. It reuses transpose and LDLT factors and follows the
CPU backend's two-vector reuse idea without adopting a CPU loop layout.

- Full-load A/B: median frame time 4.325 -> 3.993 ms (-7.66%); accumulated
  inverse time fell 18.3%.
- 64-candidate counter: 2.71 -> 1.81 ms (-33.2%), 636.56M -> 418.38M
  instructions, 251.07M -> 191.78M L1 sectors, and 59.93M -> 44.07M L2
  sectors. The paired kernel uses 48 registers and has no local spill.
- A separate single-column entry point is retained for combined-axis tiles.
  This isolates the paired kernel's register footprint; combined frame time is
  unchanged within 0.2% in alternating A/B runs.

### One-tile combined batch

Raising the default workspace cap from 384 MiB to 640 MiB lets this 64-candidate
workload use one actual 562.5 MiB workspace tile. It reduces launches per frame
from 9 to 5.

- Concurrency 1: 10.684 -> 9.672 ms; 5,990 -> 6,617 candidates/s.
- Concurrency 16: 7,888 -> 8,417 candidates/s.
- The cap is not a reservation. Explicit `workspace_limit_elements` still
  provides a lower-memory tiled mode.

## Rejected Experiments

The following changes were removed after measurement:

- Full-frame transpose for combined analysis: 15.19 ms versus 12.16 ms and
  about 288 MiB of extra scratch.
- Two adjacent horizontal outputs per thread: median 5.325 versus 5.300 ms;
  long-scoreboard and profiler duration both increased.
- F6 unrolling inside the combined fused metric: 206.72 -> 213.57 us because
  registers rose 40 -> 48 and achieved occupancy fell 96.82% -> 80.95%.
- 32-thread inverse blocks: horizontal rose to 7.30 ms and combined inverse
  also regressed.
- A 16-column local-transpose tile: combined median 10.722 -> 10.918 ms and
  inverse time rose 3.36% despite better small-grid profiler occupancy.
- A 512 MiB workspace cap: it retained two highly unbalanced tiles and slowed
  combined analysis to about 12.77 ms. Balanced two-tile limits did not beat
  the one-tile 640 MiB configuration.

## Promotion Decision

The backend can enter and remain in the profiling stage. Counters are readable,
all current hot kernels have no local spill, full-load throughput is stable,
and correctness covers horizontal, vertical, combined, mixed, tiled,
concurrent, cached, and paired-tail paths.

Handwritten PTX is not yet authorized as the production path. The next stage
must keep `cpp-generic` forceable, introduce one SM-specific operation at a
time, and require real-device correctness plus full-load A/B evidence before a
runtime variant is opened.
