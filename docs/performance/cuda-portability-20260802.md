# CUDA SM75+ Portability Evidence - 2026-08-02

## Scope

This report covers the first portability stage of the rebuilt CUDA backend.
The CUDA C++ kernel ABI and dataflow are unchanged; this stage adds a native
architecture matrix, an SM75 compatibility floor, PTX fallbacks, adaptive
device-memory budgeting, and artifact gates.

Verification toolchain and host:

- CUDA Toolkit 13.3, MSVC 19.44, Release
- NVIDIA GeForce RTX 5080, compute capability 12.0
- Native targets: SM75, 80, 86, 87, 88, 89, 90, 100, 103, 110, 120, 121
- PTX targets: compute_75 and compute_121

## Build And Artifact Gates

The complete multi-architecture fatbin builds successfully and is 17,840,008
bytes (17.013 MiB). `getnative_cuda_artifact_inventory` verifies every
configured cubin and PTX entry and rejects any native kernel with a non-zero
stack frame or local-memory allocation.

A separate SM75 compile probe produced all nine kernels without stack or local
memory. Register allocation differs from SM120 and therefore remains a
real-device profiling concern:

| Kernel | SM75 registers | Stack bytes | Local bytes |
| --- | ---: | ---: | ---: |
| `getnative_cuda_transpose_source` | 24 | 0 | 0 |
| `getnative_cuda_inverse_horizontal` | 42 | 0 | 0 |
| `getnative_cuda_horizontal_fused` | 41 | 0 | 0 |
| `getnative_cuda_inverse_vertical` | 44 | 0 | 0 |
| `getnative_cuda_inverse_vertical_pair` | 59 | 0 | 0 |
| `getnative_cuda_forward_intermediate` | 38 | 0 | 0 |
| `getnative_cuda_both_fused_metric` | 63 | 0 | 0 |
| `getnative_cuda_metric_partials` | 64 | 0 | 0 |
| `getnative_cuda_metric_finalize` | 25 | 0 | 0 |

## Runtime Gates

A reduced fatbin containing only native SM75 plus compute_75/compute_121 PTX
was loaded on the SM120 host. Because no SM120 cubin was present, the driver
used the compute_75 fallback. Artifact inventory and the full CUDA baseline
correctness, paired-tail, cache, tiling, and concurrency tests passed 2/2.

The default multi-architecture build passed 10/10 CTest tests, including the
artifact inventory and real-device CUDA baseline. Its full combined-axis,
64-candidate, concurrency-16 smoke run retained one 562.5 MiB tile and measured
7.608 ms/frame and 8,411.76 candidates/s. The adaptive policy preserved the
640 MiB workspace ceiling on the verification host.

## Memory Policy

At context creation the runtime snapshots free device memory, reserves the
larger of 512 MiB or one eighth of that snapshot (capped at half of currently
free memory), and divides the remainder across execution slots. Workspace
receives at most four fifths of each slot's budget and remains capped at 640 MiB
unless the caller requests a lower limit. The complete predicted device working
set and retained buffer capacities must fit both the per-slot budget and the
2 GiB explicit-allocation guard before allocation.

## Remaining Boundary

This stage establishes build and functional portability to SM75. It does not
claim SM75 throughput parity or optimal launch parameters. Production
performance promotion on Turing still requires the same full-load profile and
A/B process used for SM120.
