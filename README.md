# GetNative VF

Standalone native-resolution analysis project. The product does not load or
require VapourSynth, Python, plugins, or `.vpy` scripts at runtime.

## Current Status

- `getnative-engine` is a C++23 CLI with compatibility profiles, candidate-grid
  semantics, Python-compatible rounding, and fractional crop geometry.
- `getnative_core` now implements Bilinear, Bicubic, Lanczos, Spline16/36/64,
  JET-compatible positive `blur` scales, direct banded `A^T A` planning,
  Float32 descale/forward execution, fused
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
- The GUI exposes the real geometry command and validates capability schema v2.
  It reports compile, device, and analysis-command availability separately and
  does not display placeholder curves, valleys, or analysis controls while the
  `analyze` command is unavailable. Decode, the long-running worker protocol,
  CUDA, exports, and real GUI analysis remain incomplete.
- The optional Windows/Linux Vulkan 1.2 backend implements horizontal,
  vertical, and combined-axis strict p=1 analysis through half-bandwidth 15 /
  forward width 16. It embeds validated SPIR-V, dynamically loads the platform
  loader, uses fixed b1/3/5/7/9/11 inverse pipelines plus a generic path, and
  caches device-resident packed plans with pooled timeline uploads and a fence
  fallback. Capability availability requires pipeline creation and a runtime
  CPU-oracle conformance self-test.

## Supported Build Targets

| Platform | CPU | GPU backend | CI |
| --- | --- | --- | --- |
| macOS ARM64 (`macos-15`) | Yes, NEON | Metal | `engine-macos.yml` |
| Linux x86_64 (`ubuntu-24.04`) | Yes | Vulkan when a device is available | `engine-linux.yml` |
| Linux ARM64 (`ubuntu-24.04-arm`) | Yes, NEON | Vulkan when a device is available | `engine-linux.yml` |
| Windows x86_64 | Yes | CUDA/Vulkan via the remote-machine workflow | Manual/remote |

Linux Vulkan CI deliberately uses the distro toolchain (`glslang-tools`,
`spirv-tools`, and `libvulkan-dev`). LunarG does not publish a Linux ARM64
SDK. `glslangValidator` is accepted as the shader compiler fallback for
`glslc`; SPIR-V is an architecture-independent artifact. CI validates
`spirv-val` offline and skips the runtime Vulkan test with its existing
`SKIP:` contract when no compute device is present. GPU kernels and analysis
code contain no platform macros; platform selection is confined to CMake and
the small loader shim.

## Blur Contract

Every filter accepts `blur` with default `1.0`. The library requires a finite,
positive value; the GUI contract starts at `0.75` to match muvsfunc. Planning
uses `ceil(base_support * blur)` and evaluates weights at `distance / blur`.
Lanczos keeps its original taps as the window parameter. The exact `1.0` path
does not divide and preserves the previous plan bits and cache identity.

Both the inverse/descale matrix and the retained zimg-style forward projection
use the stretched kernel. This matches muvsfunc's intent of applying blur to
both descale and rescale (`fmtc` uses `fv=fh=1/blur`), while GetNative-VF keeps
its existing zimg geometry rather than switching the forward path to fmtconv.

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

# Windows/Linux Vulkan conformance and fixed-kernel A/B gate.
cmake -S engine -B build/engine-vulkan -DCMAKE_BUILD_TYPE=Release \
  -DGETNATIVE_ENABLE_METAL=OFF -DGETNATIVE_ENABLE_VULKAN=ON
cmake --build build/engine-vulkan --parallel
ctest --test-dir build/engine-vulkan --output-on-failure
build/engine-vulkan/getnative_vulkan_kernel_benchmark --assert

cd app
npm run build
cargo test --manifest-path src-tauri/Cargo.toml
cargo clippy --manifest-path src-tauri/Cargo.toml --all-targets -- -D warnings
npm run tauri build -- --bundles app
```

On Apple platforms CMake enables Metal when both `metal` and `metallib` are
installed. If the optional MetalToolchain component is unavailable it emits a
warning and produces the CPU-only engine instead. Linux and Windows may enable
Vulkan explicitly with `-DGETNATIVE_ENABLE_VULKAN=ON`; Vulkan headers, `glslc`,
`spirv-val`, and `spirv-dis` are build-only requirements. The resulting binary
does not import `vulkan-1.dll` or `libvulkan`; it resolves the loader at runtime.
