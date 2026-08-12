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
  and combined-axis p=1..4 analysis. It uses persistent execution slots, pinned
  staging, resident plan caches, candidate tiling, axis-specific kernels,
  source/tile transposes, and fused reconstruction/metric passes. The public
  variant remains `cpp-generic`; unsupported specialization and inline-PTX
  variants fail closed.
- `verify_media_begin` indexes and decodes video inside the engine through the
  FFmpeg 8 shared-library ABI. CPU uses software decode. CUDA shares the
  analysis `CUcontext` with FFmpeg for direct NVDEC NV12/P010-to-F32 input;
  Vulkan shares one `VkInstance`/`VkDevice`, queues, and timeline semaphore
  state with FFmpeg for Vulkan Video decode. Unsupported hardware decode emits
  one structured fallback warning and continues with software decode plus the
  selected compute backend.
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
  backend without adding a driver import to CPU-only startup. The legacy
  FFmpeg/mmap verify ring remains available when engine media decode is absent.

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

# Debian/Ubuntu development metadata for the optional in-engine media layer.
# These pkg-config modules must resolve to the FFmpeg 8 ABI.
sudo apt install pkg-config libavformat-dev libavcodec-dev libavutil-dev libswscale-dev

# Linux Vulkan build and validation tools. A recent LunarG SDK may be used
# instead by exporting VULKAN_SDK before configuring.
sudo apt install libvulkan-dev glslc spirv-tools vulkan-tools vulkan-validationlayers

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

# Windows/Linux CUDA correctness, NVDEC bridge, and throughput verification.
cmake -S engine -B build/engine-cuda -DGETNATIVE_ENABLE_CUDA=ON \
  -DGETNATIVE_CUDA_MIN_ARCHITECTURE=75
cmake --build build/engine-cuda --config Release --parallel
ctest --test-dir build/engine-cuda -C Release --output-on-failure
build/engine-cuda/Release/getnative_cuda_throughput_benchmark --full \
  --axes both --concurrency 1

# Linux Vulkan compute and Vulkan Video bridge verification.
cmake -S engine -B build/engine-vulkan -DGETNATIVE_ENABLE_VULKAN=ON
cmake --build build/engine-vulkan --parallel
ctest --test-dir build/engine-vulkan --output-on-failure

cd app
npm run build
cargo test --manifest-path src-tauri/Cargo.toml
cargo clippy --manifest-path src-tauri/Cargo.toml --all-targets -- -D warnings
npm run tauri build -- --bundles app
```

`GETNATIVE_ENABLE_MEDIA=ON` is the default. If the FFmpeg 8 development
metadata is missing, a development build keeps image workflows available and
reports video as unavailable. There is no external-FFmpeg fallback. Release
packaging requires the in-process media commands and stages only the pinned
LGPL `avformat`/`avcodec`/`avutil`/`swscale` shared libraries beside the engine;
`ffmpeg` and `ffprobe` executables are never packaged or launched at runtime.
On macOS, `npm run stage:ffmpeg:macos` prepares the pinned libraries in
`src-tauri/ffmpeg-runtime`; subsequent engine/package builds consume that
directory automatically.

Vulkan compute needs headers, a loader, and `glslc` or
`glslangValidator`; test builds also require `spirv-val`. Vulkan Video is
advertised at runtime only when the selected device exposes Vulkan 1.3, a
decode queue, timeline semaphores, and a supported H.264/HEVC/AV1/VP9 decode
extension. Missing video capability falls back to software decode plus Vulkan
upload, so it does not disable Vulkan compute or media verification.

On Linux, `npm run build:engine` uses `GETNATIVE_LINUX_BACKENDS=auto` by
default. `auto` enables CUDA when a toolkit containing `bin/nvcc` is found in
`GETNATIVE_CUDA_ROOT`, `CUDAToolkit_ROOT`, `CUDA_PATH`, `CUDA_HOME`, or
`/usr/local/cuda`; otherwise it builds CPU-only. Use `cpu`, `cuda`, `vulkan`, or
`cuda-vulkan` to choose explicitly. For a release package, point
`GETNATIVE_FFMPEG_RUNTIME_DIR` at the pinned FFmpeg 8 shared-library directory.
The build stages only the ABI-matched avformat 62, avcodec 62, avutil 60, and
swscale 9 libraries beside the engine and records every file hash in
`build-provenance.json`. For example:

```bash
cd app
VULKAN_SDK=/path/to/vulkan-sdk/x86_64 \
GETNATIVE_LINUX_BACKENDS=cuda-vulkan \
GETNATIVE_FFMPEG_RUNTIME_DIR=/usr/lib/x86_64-linux-gnu \
npm run build:engine
```

Omit `VULKAN_SDK` when the Vulkan headers, loader, shader compiler, validation
layers, and SPIR-V tools are installed in standard system paths. The packaged
engine reports `features.verify_engine_decode` and the `software`, `nvdec`,
and `vulkan_video` entries in `decode_backends`; the GUI uses the legacy ring
when engine decode is not compiled.

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
`analysis_command_available` separately. The resident worker exposes analysis
when its backend and device are available; one-shot CLI capability discovery
continues to report the analysis command as unavailable.

On Windows, `npm run build:engine` and the Tauri package build use
`GETNATIVE_WINDOWS_BACKENDS=cpu` by default. Set it to `cuda` to build and test
the CUDA backend explicitly. Set `GETNATIVE_CUDA_ARCHITECTURES` when
building a reduced native-code matrix, and keep the configured minimum in both
the native and PTX lists. `GETNATIVE_VSDEVCMD`, `CMAKE`, and `CTEST` can
override tool locations. The build script configures the requested backend
set, builds it, runs CTest, installs that exact engine, and writes its SHA-256
and complete CUDA target provenance into the package resources.
