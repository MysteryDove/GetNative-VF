# Vulkan analysis port (2026-08-10)

## Scope and decision

- Host: Linux x86-64, NVIDIA GeForce RTX 5080, driver 595.84, Release build.
- Decision: **ADOPT** as an explicit, build-time optional analysis backend.
- Scope: port the mature Vulkan device, buffer, descriptor, command, and compute
  patterns from Descale-MVC. Keep GetNative-VF's own 32-candidate worker chunks,
  pipeline depth, progress reporting, and cancellation behavior.
- Compatibility: explicit Vulkan supports `h_only`, `w_only`, and `h_plus_w`
  with p=1. `auto` remains CUDA then CPU, and verification remains CPU-only.

The implementation uses two persistent execution slots, grow-to-fit staging and
device buffers, a CUDA-compatible packed plan layout, tiled transpose, a
15-deep register-ring inverse, a forward intermediate, and strict p=1 metric
reduction. Candidate work is additionally tiled to satisfy the configured
workspace limit and `maxStorageBufferRange`. GPU F32 partials are accumulated
in deterministic order with CPU double precision.

## Correctness gate

The Vulkan Release build passed all 16 CTest entries. This includes the CPU,
SIMD, planner, store, worker-protocol, CLI, and Vulkan conformance suites, plus
`spirv-val --target-env vulkan1.2` for all four embedded shader modules.

The device conformance run covered all three axis modes, crop and threshold,
tail and multi-tile shapes, minimum selection, and a strict-threshold equality
case. It also passed with Vulkan validation enabled and the forced noncoherent
host-memory path; reported validation errors were zero. The worker protocol
covered a 40-candidate, two-chunk CPU parity run, p>1 rejection, telemetry, and
explicit verification rejection.

## Worker benchmark

The benchmark exercised the real worker path in one process so the resident
Vulkan engine could be measured cold and warm:

- Input: locally generated GRAY F32, 1920x1080.
- Search: 301 candidates, 760.00 through 835.00 in 0.25 steps.
- Recipe: `h_plus_w`, Lanczos8, crop 10, threshold 0.015, p=1.
- Timing: three CPU runs followed by three Vulkan runs; candidate time is
  worker stage telemetry and wall time includes plan construction.

| Backend / run | Candidate ms | Wall ms | Plan ms |
| --- | ---: | ---: | ---: |
| CPU 1 | 4825.337 | 4945.051 | 112.169 |
| CPU 2 | 4780.397 | 4874.556 | 93.357 |
| CPU 3 | 4765.799 | 4862.121 | 95.503 |
| Vulkan 1 (cold) | 386.534 | 481.106 | 93.760 |
| Vulkan 2 (warm) | 151.895 | 247.332 | 94.573 |
| Vulkan 3 (warm) | 133.197 | 235.232 | 101.194 |

The candidate-phase medians are 4780.397 ms for CPU and 151.895 ms for
Vulkan, a **31.47x** speedup. The first Vulkan run includes runtime, pipeline,
and buffer initialization; the following runs demonstrate reuse of the
resident engine and grow-to-fit allocations.

All 301 Vulkan metrics matched CPU within
`max(2e-7, 5e-4 * abs(cpu_metric))`. The maximum absolute difference was
`2.299e-11`; both backends selected candidate index 294. These figures are a
device-specific implementation gate, not a universal Vulkan performance claim.

## Build and packaging

Vulkan is disabled by default and enabled with `GETNATIVE_ENABLE_VULKAN=ON`
(or `GETNATIVE_ENABLE_VULKAN=1` through the app build script). Windows package
selection accepts `vulkan` and `cuda-vulkan` in addition to the existing CPU
and CUDA values. Linux and Windows builds require a Vulkan loader and either
`glslc` or `glslangValidator` to compile the shaders.
