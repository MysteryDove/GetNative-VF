# RTX 5080 Candidate-Scan Snapshot

## Scope

This document records the engine-level synthetic candidate scans run on
2026-08-27. It is a development snapshot for checking multi-tile scheduling,
warm plan reuse, and approximate throughput. It is not a media pipeline or
VapourSynth end-to-end benchmark.

Host:

- CPU: AMD Ryzen 9 5950X, 16 cores / 32 threads.
- GPU: NVIDIA GeForce RTX 5080, 16 GiB.
- NVIDIA driver: 595.84.
- OS: Linux 7.0.0-30-generic x86_64.
- Compiler: GCC 15.2.0, Release build.

## Vulkan Scan

The Vulkan scan used the current uncommitted Vulkan worktree based on
`d64af6c`. The relevant local sources were copied unchanged to
`/tmp/getnative-vf/src` and built in
`/tmp/getnative-vf/build-vulkan-dynamic` with `GETNATIVE_ENABLE_VULKAN=ON`.
Shaders were built with shaderc/SPIR-V Tools v2026.3 and embedded in the
executable.

Command:

```sh
/tmp/getnative-vf/build-vulkan-dynamic/getnative_vulkan_kernel_benchmark \
  --scan --assert
```

Workload:

- Source: deterministic synthetic Float32 `1920x1080` image.
- Axis: vertical.
- Filter: Bicubic Catrom.
- Active height: evenly distributed over `800.0..899.9`.
- Metric: crop 5 on every side, threshold `0.015`, p=1.
- Tile limit: 32 candidates.
- Samples: one cold call followed by three warm calls; the table reports the
  median warm GPU fence duration.

| Candidates | Unique plans | Tiles | Planner ms | Cold GPU ms | Warm GPU ms | Candidates/s | Cold plan MiB | Peak workspace MiB |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 64 | 2 | 2.43 | 4.47 | 4.04 | 15,841 | 5.09 | 205.03 |
| 256 | 256 | 8 | 6.77 | 15.92 | 15.09 | 16,968 | 20.36 | 209.37 |
| 1000 | 1000 | 32 | 24.64 | 61.59 | 58.43 | 17,115 | 79.54 | 210.26 |

The assert gate confirmed that every warm call hit the prepared-plan cache,
uploaded zero plan bytes, used one fixed-bandwidth specialized inverse dispatch
per tile, and completed every submission. Three CPU-oracle samples per scan had
a maximum absolute error of `6.79e-13`; the sampled metric range was
`0..0.000170556`, so the result was not trivially cleared by the threshold.
The complete Vulkan Release test run passed `29/29` tests.

Executable SHA-256:

```text
6a5465c6aba7a39b714c91a1cb2677e3a6f7f1a2dc80610b08c1b766ceb90807
```

## CUDA Scan

The CUDA scan used the clean remote GetNative-VF CUDA baseline at `9f3a425`,
copied to `/tmp/getnative-cuda-scan.x0FF0H/src`. It was built with CUDA 13.3 as
the staged generic C++ implementation. The fatbin contains native
`sm_75`, `sm_86`, `sm_89`, and `sm_120` code plus `compute_75` and
`compute_120` PTX.

Configure and build:

```sh
cmake -S /tmp/getnative-cuda-scan.x0FF0H/src/engine \
  -B /tmp/getnative-cuda-scan.x0FF0H/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DGETNATIVE_ENABLE_CUDA=ON \
  -DGETNATIVE_ENABLE_VULKAN=OFF \
  -DGETNATIVE_ENABLE_METAL=OFF \
  -DGETNATIVE_ENABLE_MEDIA=OFF \
  -DGETNATIVE_CUDA_ROOT=/usr/local/cuda-13.3

cmake --build /tmp/getnative-cuda-scan.x0FF0H/build \
  --target getnative_cuda_throughput_benchmark \
           getnative_cuda_baseline_tests -j 16
```

Benchmark command, repeated with `N=64`, `256`, and `1000`:

```sh
/tmp/getnative-cuda-scan.x0FF0H/build/getnative_cuda_throughput_benchmark \
  --width 1920 --height 1080 \
  --native-width 1280 --native-height 850 \
  --axes vertical --candidates N --warmups 3 --samples 7
```

Workload:

- Source: deterministic synthetic Float32 `1920x1080` image.
- Axis: vertical.
- Filter: Lanczos3.
- Active height: approximately `849.5..850.5`, with candidate-dependent shift.
- Metric: crop 8 on every side, threshold `0`, p=1.
- Workspace limit: automatic, effective limit 640 MiB.
- Samples: three warmups followed by seven measured calls; the table reports
  median end-to-end wall duration.

| Candidates | Tiles/call | Wall median ms | MAD ms | Candidates/s | Peak workspace MiB | Peak working set MiB |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 1 | 3.116 | 0.008 | 20,538 | 398.44 | 413.91 |
| 256 | 3 | 13.226 | 0.035 | 19,356 | 635.01 | 673.03 |
| 1000 | 10 | 52.308 | 0.045 | 19,118 | 635.01 | 760.23 |

The measured calls had plan-cache and source-cache hits with zero source or
plan upload bytes. Tile counts in the raw telemetry were `7`, `21`, and `70`
across seven samples, corresponding to `1`, `3`, and `10` tiles per call. The
CUDA generic baseline test passed on the RTX 5080 before the scan.

Artifact SHA-256 values:

```text
getnative_cuda_throughput_benchmark  5d7f2898ed319ee1a16ec4218662aa2d57fed8dd0950a9624e28baa92d7e12dc
getnative_cuda_staged.fatbin         5e664f4bd06dc21486fe6ef72fade0cb4fe8507906f35d0e2b8936cc29655d9e
```

## Comparison Boundary

The Vulkan and CUDA figures are not a controlled backend A/B. The fixtures use
different filters, active-height ranges, metric crops and thresholds, tile
policies, and timing boundaries. Vulkan reports the warm GPU fence duration;
CUDA reports end-to-end wall duration. CUDA is also the generic baseline,
whereas Vulkan automatically selected its fixed-bandwidth specialized inverse
pipeline. Use the values only as separate backend scaling snapshots.

A valid CUDA/Vulkan comparison must share the exact source pixels, `AxisPlan`
objects, candidate order, metric, workspace/tile policy, warmup/sample schedule,
and timing boundary in one benchmark protocol.
