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
- CPU plans are byte-conformant with the pinned descale inverse and zimg
  forward references in the optional upstream conformance suite. The product
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
- `getnative-gui` is a Tauri 2 + React/TypeScript workbench. Rust owns the
  fixed engine process boundary; the webview has no general shell permission.
- macOS development and `.app` packaging automatically build and include the
  engine under `Contents/Resources/bin`.
- The GUI exposes the real geometry command and validates capability schema v3.
  It reports compile, device, and analysis-command availability separately and
  does not display placeholder curves, valleys, or analysis controls while the
  `analyze` command is unavailable. Windows builds can embed optional CUDA and
  Vulkan compute backends without adding loader/driver imports to CPU-only
  startup. Decode, the long-running worker protocol, exports, and real GUI
  analysis remain incomplete.

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

# Exact test-only comparisons with the pinned standalone upstream cores.
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

The Windows engine has four explicit CMake switches:

- `GETNATIVE_ENABLE_X86_SIMD` builds the runtime-dispatched SSE2 and AVX2 tiers.
- `GETNATIVE_ENABLE_X86_AVX512` additionally builds the forced experimental
  AVX-512 tier; runtime CPUID/XCR0 checks still decide whether it is available.
- `GETNATIVE_ENABLE_CUDA` embeds a strict CUDA fatbin plus forward-compatible
  PTX. `GETNATIVE_CUDA_ARCHITECTURES` is the semicolon-separated native target
  list; its last entry also supplies the PTX target.
- `GETNATIVE_ENABLE_VULKAN` embeds validated Vulkan 1.2 SPIR-V shaders.

CUDA and Vulkan use runtime-loaded driver APIs. A package built with either
backend still starts without the corresponding loader or driver and reports
`compiled`, `device_available`, and `analysis_command_available` separately.
The product has no analysis worker yet, so `commands.analyze` and every
`analysis_command_available` field remain `false`.

On Windows, `npm run build:engine` and the Tauri package build use
`GETNATIVE_WINDOWS_BACKENDS=both` by default. Set it to `cpu`, `cuda`, or
`vulkan` for an explicit reduced build. Set
`GETNATIVE_CUDA_ARCHITECTURES` when targeting a device other than the default
`120`; `GETNATIVE_VSDEVCMD`, `CMAKE`, and `CTEST` can override tool locations.
The build script configures the requested backend set, builds it, runs CTest,
installs that exact engine, and writes its SHA-256 and backend provenance into
the package resources.
