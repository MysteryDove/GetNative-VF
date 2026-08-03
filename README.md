# GetNative VF

Standalone native-resolution analysis project. The product does not load or
require VapourSynth, Python, plugins, or `.vpy` scripts at runtime.

## Current Status

- `getnative-engine` is a C++23 CLI with compatibility profiles, candidate-grid
  semantics, Python-compatible rounding, and fractional crop geometry.
- `getnative_core` now implements Bilinear, Bicubic, Lanczos, Spline16/36/64,
  direct banded `A^T A` planning, Float32 descale/forward execution, fused
  thresholded p-norm metrics, single-axis analysis, and deterministic parallel
  candidate batches.
- CPU planner coefficients, sparse topology, and LDLT factors are bitwise
  conformant with the pinned descale inverse and zimg forward references in the
  optional upstream conformance suite. Final CPU outputs are numerically
  equivalent under a strict absolute/relative tolerance; production FMA/SIMD
  paths may be more accurate than the unfused scalar reference. The product
  runtime does not link either reference implementation.
- `getnative_metal` supports horizontal, vertical, and combined-axis strict p=1
  analysis. It dispatches specialized B3/B7 kernels plus a generic path through
  half-bandwidth 15 / forward width 16, batches mixed plan shapes in stable
  order, reuses one bounded two-axis arena, and fuses the final reconstruction
  axis into metric reduction.
- On the Apple M4 Max verification host, the 1920x1080/1000-candidate vertical
  bicubic benchmark has a five-run Metal median of 125.409 ms, stays within the
  strict CPU metric bound at 5.61e-8 maximum error, selects the same valley, and
  uses 168.750 MiB peak arena workspace and 252.230 MiB across all explicit
  Metal buffers.
- `getnative_cuda` is a rebuilt Driver-API backend for horizontal, vertical,
  and combined-axis p=1 analysis. It uses persistent execution slots, pinned
  staging, resident plan caches, candidate tiling, axis-specific kernels,
  source/tile transposes, and fused reconstruction/metric passes. The public
  variant remains `cpp-generic`; unsupported specialization and inline-PTX
  variants fail closed.
- On the RTX 5080 verification host, the 1920x1080 -> 1280x720, 64-candidate
  benchmark has five-run medians of 4.025 ms vertical, 5.028 ms horizontal, and
  9.676 ms combined at concurrency one. At concurrency 16 it reaches 17.45k,
  15.55k, and 8.41k candidates/s respectively. The combined path uses one
  562.5 MiB workspace tile under the 640 MiB default limit.
- `getnative-gui` is a Tauri 2 + React/TypeScript workbench. Rust owns the
  fixed engine process boundary; the webview has no general shell permission.
- macOS development and `.app` packaging automatically build and include the
  engine under `Contents/Resources/bin`.
- The GUI exposes the real geometry command and validates capability schema v2.
  It reports compile, device, and analysis-command availability separately and
  does not display placeholder curves, valleys, or analysis controls while the
  `analyze` command is unavailable. Windows builds can embed the optional CUDA
  backend without adding a driver import to CPU-only startup. Decode, the
  long-running worker protocol, exports, and real GUI analysis remain
  incomplete.

The accepted architecture and implementation sequence are documented in
`docs/architecture.md` and `.omx/plans/standalone-getnative.md`.

## Run

```sh
cd app
npm install
npm run tauri dev
```

The dev command builds and stages the C++ engine before starting Tauri.

## Publication Status

GetNative VF is licensed under the MIT License. See `LICENSE` for details.
`THIRD_PARTY_NOTICES.md` covers the pinned conformance references; the
real-image benchmark fixture remains local-only until redistribution rights are
established.

## Verify

```sh
cmake -S engine -B build/engine -DCMAKE_BUILD_TYPE=Debug
cmake --build build/engine --parallel
ctest --test-dir build/engine --output-on-failure

# Planner bitwise and final-output numerical comparisons with the pinned cores.
cmake -S engine -B build/engine-conformance -DCMAKE_BUILD_TYPE=Release \
  -DGETNATIVE_BUILD_UPSTREAM_CONFORMANCE=ON
cmake --build build/engine-conformance --parallel
ctest --test-dir build/engine-conformance --output-on-failure

# Planner and end-to-end CPU performance report with assertions.
build/engine-conformance/getnative_core_benchmark --assert

# macOS Metal conformance and the full 1080p/1000-candidate gate.
cmake -S engine -B build/engine-metal -DCMAKE_BUILD_TYPE=Release \
  -DGETNATIVE_ENABLE_METAL=ON
cmake --build build/engine-metal --parallel
ctest --test-dir build/engine-metal --output-on-failure
build/engine-metal/getnative_metal_benchmark --full --assert

# Windows CUDA correctness and throughput verification.
cmake -S engine -B build/engine-cuda -DGETNATIVE_ENABLE_CUDA=ON \
  -DGETNATIVE_CUDA_MIN_ARCHITECTURE=75
cmake --build build/engine-cuda --config Release --parallel
ctest --test-dir build/engine-cuda -C Release --output-on-failure
build/engine-cuda/Release/getnative_cuda_throughput_benchmark --full \
  --axes both --concurrency 1

cd app
npm run build
cargo test --manifest-path src-tauri/Cargo.toml
cargo clippy --manifest-path src-tauri/Cargo.toml --all-targets -- -D warnings
npm run tauri build -- --bundles app
```

On Apple platforms CMake enables Metal when both `metal` and `metallib` are
installed. If the optional MetalToolchain component is unavailable it emits a
warning and produces the CPU-only engine instead. Other platforms are CPU-only
unless a later backend is enabled explicitly.

## Windows Backends

The Windows engine has three explicit CMake switches:

- `GETNATIVE_ENABLE_X86_SIMD` builds the runtime-dispatched SSE2 and AVX2 tiers.
- `GETNATIVE_ENABLE_X86_AVX512` additionally builds the forced experimental
  AVX-512 tier; runtime CPUID/XCR0 checks still decide whether it is available.
- `GETNATIVE_ENABLE_CUDA` embeds the profiled `cpp-generic` CUDA C++ backend in
  a multi-architecture fatbin. The default package contains native code for
  SM75, 80, 86, 87, 88, 89, 90, 100, 103, 110, 120, and 121, plus compute_75
  and compute_121 PTX fallbacks. `GETNATIVE_CUDA_MIN_ARCHITECTURE`,
  `GETNATIVE_CUDA_ARCHITECTURES`, and `GETNATIVE_CUDA_PTX_ARCHITECTURES`
  configure those three independent boundaries.
  Common B3/B5 and F6 shapes have local compile-time specializations inside
  this variant; architecture-specific and handwritten-PTX runtime variants are
  intentionally unavailable until they pass separate correctness and A/B gates.

CUDA uses the runtime-loaded Driver API. A CUDA-enabled package still starts
without an NVIDIA driver and reports `compiled`, `device_available`, and
`analysis_command_available` separately. The product has no analysis worker
yet, so `commands.analyze` and every `analysis_command_available` field remain
`false`.

On Windows, `npm run build:engine` and the Tauri package build use
`GETNATIVE_WINDOWS_BACKENDS=cpu` by default. Set it to `cuda` to build and test
the CUDA backend explicitly. Set `GETNATIVE_CUDA_ARCHITECTURES` when
building a reduced native-code matrix, and keep the configured minimum in both
the native and PTX lists. `GETNATIVE_VSDEVCMD`, `CMAKE`, and `CTEST` can
override tool locations. The build script configures the requested backend
set, builds it, runs CTest, installs that exact engine, and writes its SHA-256
and complete CUDA target provenance into the package resources.
