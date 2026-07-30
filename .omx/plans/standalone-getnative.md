# Standalone GetNative Delivery Plan

## Requirements Summary

- Deliver an independent desktop GUI and independent headless backend.
- Runtime must not depend on VapourSynth, its plugins, Python, or `.vpy`.
- Preserve the useful fractional-resolution, parity, height, kernel, frame, and
  error-plot semantics of the training material and `muvsfunc.getnative`.
- Accelerate candidate evaluation with batched Metal and CUDA backends.
- Keep a deterministic CPU reference backend and explicit compatibility
  profiles.
- Decode images and video directly, export structured results and equivalent
  plots, and support cancellable batch jobs.

The canonical design is `docs/architecture.md`.

## Assumptions

- macOS arm64/Metal is the first implementation and validation target.
- Windows x64/CUDA is the second target; Linux x64/CUDA follows the same worker
  and backend contract.
- Software decode is the strict default. Hardware decode is a later opt-in
  because it can change pixel values.
- Upstream repositories are behavioral/mathematical references only except for
  permissively licensed standalone components that are deliberately adopted.

## Acceptance Criteria

1. A dependency and binary scan finds no VapourSynth symbols, Python runtime,
   plugin loader, or `.vpy` execution path in product builds.
2. `getnative-engine capabilities` and JSONL `hello` work without the GUI and
   report CPU plus available optional GPU backends.
3. Geometry tests reproduce the course examples for `1488x837`, even/odd base
   dimensions, `src_top=0.5`, and `src_left=0.5` exactly.
4. Candidate generation reproduces both MUF repeated-addition and GetFnative
   index-multiplication grids; the modern profile uses decimal fixed point.
5. CPU strict results pass frozen geometry, coefficient, pixel, metric, valley,
   and plot-data fixtures without any runtime VS dependency.
6. Metal strict and CUDA strict pass the numeric and valley-equivalence gates in
   `docs/architecture.md` on real target hardware.
7. A 1920x1080/1000-candidate job stays under 512 MiB planner RSS and 2 GiB GPU
   working memory.
8. A GPU backend is enabled by default only after achieving at least 3x the
   optimized CPU backend on the same host and benchmark.
9. The GUI can open an image or video, select a frame, configure all core
   parameters, run/cancel a job, inspect valleys and geometry, and export JSON,
   CSV, legacy TXT, SVG, and PNG.
10. Worker crashes, decoder failures, missing GPU drivers, and cancellation are
    visible in the GUI and do not corrupt saved results.
11. macOS release packaging includes the engine, notices, signing, and
    notarization; Windows/Linux packages do not require CUDA to start.

## Implementation Steps

### 1. Freeze Reference Contracts

Create:

- `docs/compatibility.md`
- `protocol/job-v1.schema.json`
- `protocol/result-v1.schema.json`
- `fixtures/reference/manifest.json`

Record the pinned upstream revisions, exact geometry rounding, grid semantics,
error metric, plot epsilon, color assumptions, and license boundary. Extract or
create only redistributable fixtures; do not copy unlicensed MUF source.

Verification:

- Schema validation tests cover every command/result example.
- A source inventory script rejects files copied from unapproved upstream
  paths.

### 2. Create The Engine And Geometry Core

Create C++23/CMake targets:

- `engine/include/getnative/types.hpp`
- `engine/src/geometry/profile.cpp`
- `engine/src/geometry/candidate_grid.cpp`
- `engine/src/geometry/crop_geometry.cpp`
- `engine/tests/geometry_test.cpp`

Implement Python round-to-even, Python positive-value truncation, decimal
candidate representation, and the three compatibility profiles before any GPU
code.

Verification:

- Course parity examples and half-tie rounding are exact.
- Sanitizer builds pass on macOS/Linux CPU jobs.

### 3. Implement The Standalone Planner And CPU Oracle

Create:

- `engine/src/planner/filter.cpp`
- `engine/src/planner/axis_plan.cpp`
- `engine/src/planner/banded_ldlt.cpp`
- `engine/src/backend/cpu/cpu_executor.cpp`
- `engine/tests/planner_golden_test.cpp`
- `engine/tests/cpu_golden_test.cpp`

Adapt the MIT descale mathematics with notice preservation. Retain the forward
matrix `A`, construct `A^T A` directly in banded storage, and preserve reference
Float32 solve order. Implement strict threshold/crop/p-norm reduction.

Verification:

- Compare dense and banded planners on small random dimensions.
- Compare scalar and SIMD CPU paths.
- Match frozen external-oracle results within the declared strict bound.

### 4. Add Independent Media And Color Input

Create:

- `engine/src/media/ffmpeg_decoder.cpp`
- `engine/src/media/frame_index.cpp`
- `engine/src/color/luma.cpp`
- `engine/tests/media_fixture_test.cpp`

Use an LGPL-only FFmpeg build. Preserve frame index, best-effort timestamp,
stream metadata, range, matrix, transfer, primaries, and chroma location in
provenance. Default to software decode in strict mode.

Verification:

- Decode 8/10/12-bit YUV and RGB fixtures.
- Verify direct luma extraction and explicit RGB BT.709 conversion.
- Repeated seeks return the same frame hash.

### 5. Define The Worker And CLI

Create:

- `engine/src/cli/main.cpp`
- `engine/src/cli/jsonl_worker.cpp`
- `engine/src/scheduler/job.cpp`
- `engine/src/scheduler/candidate_tiles.cpp`
- `engine/tests/protocol_integration_test.cpp`

Implement `hello`, `capabilities`, `probe`, `analyze`, `cancel`, `export`, and
`shutdown`. Keep logs on stderr and structured messages on stdout.

Verification:

- Malformed messages fail with typed errors.
- Cancellation and worker termination leave no orphan process or partial final
  result.

### 6. Optimize And Benchmark CPU Planning

Add coefficient-plan caches and candidate grouping. Benchmark plan creation,
inverse solve, forward reconstruction, reduction, and total wall time
separately.

Verification:

- Same result hashes as the unoptimized CPU oracle.
- Planner RSS and performance gates pass.

### 7. Implement Metal MVP

Create:

- `engine/src/backend/metal/metal_backend.mm`
- `engine/src/backend/metal/getnative.metal`
- `engine/tests/metal_conformance_test.cpp`

Start with GRAY F32, bicubic `(0, 0.5)`, height sweep, crop 5, p=1, and strict
mode. Implement bandwidth-7 horizontal/vertical solves, forward taps, fused
metric reduction, candidate tiling, and cancellation.

Prerequisite:

- The Xcode MetalToolchain component is installed and offline `.metal` to AIR to
  embedded metallib compilation is verified on the current M4 Max host.

Verification:

- CPU/Metal coefficient identity and strict metric/valley tests.
- M4 Max 1000-candidate performance and memory report.
- Enable-by-default go/no-go gate.

Current MVP evidence: single-axis bandwidth-7/four-tap p=1 conformance passes;
the 1920x1080/1000-candidate vertical bicubic benchmark preserves the valley,
uses 168.75 MiB peak workspace, and measures 3.75-3.93x over the optimized CPU
backend. Full kernel/mode coverage remains step 9 work.

### 8. Build The Tauri Workbench

Create `app/` with the workflow and controls described in
`docs/architecture.md`. React/TypeScript owns presentation and a custom
SVG/Canvas plot. A small Rust controller owns the packaged engine sidecar,
validates commands, and forwards typed events. Do not expose a general shell
plugin or arbitrary command execution to the webview.

Verification:

- Component tests for long labels, error states, progress, cancellation,
  backend fallback, and result selection.
- Rust tests for sidecar path resolution, command validation, lifecycle, and
  cancellation.
- Desktop screenshots at compact laptop and wide 4K logical sizes.
- An end-to-end test launches the real engine, runs a fixture, and exports all
  formats.

### 9. Add Full Kernel And Analysis Modes

Add bilinear, arbitrary bicubic, Lanczos, Spline16/36/64, horizontal-only,
vertical-only, combined axes, kernel sweep, multi-frame verify, letterbox, and
GetFnative linear-light compatibility.

Verification:

- One golden family per kernel and mode.
- Cross-profile tests prove intentional differences rather than hiding them.

### 10. Implement CUDA Backend

Create:

- `engine/src/backend/cuda/cuda_backend.cpp`
- `engine/src/backend/cuda/getnative.cu`
- `engine/tests/cuda_conformance_test.cpp`

Use the CUDA driver API, ship fatbins plus PTX fallback, reuse the exact
`AxisPlan`, and implement the same specialized/generic kernels and strict/fast
modes as Metal.

Verification:

- Real NVIDIA runner evidence for correctness, missing-driver fallback, memory,
  cancellation, and performance.
- Do not treat compilation or emulation as CUDA runtime proof.

### 11. Export, Packaging, And Release Hardening

Create a shared structured plot model. Render JSON/CSV/TXT in the engine and
SVG/PNG through a deterministic renderer contract shared with the GUI.

Verification:

- Dependency/license manifests for every platform.
- macOS signing/notarization smoke, Windows package smoke, Linux AppImage/tar
  smoke.
- Binary dependency scan proves no VS/Python runtime and optional CUDA startup.

## Risks And Mitigations

- GPU triangular dependencies may underutilize the device. Mitigation: batch
  candidates and vectors, specialize common bandwidths, fuse reconstruction and
  reduction, and keep the 3x go/no-go gate.
- Pixels near threshold can flip across FMA/reduction orders. Mitigation: strict
  compile flags, CPU-generated coefficients, fixed partial order, explicit
  tolerance plus valley gates, and CPU compatibility fallback.
- FFmpeg decoding may differ from a historical VS source plugin. Mitigation:
  store decode/color provenance, use software decode in strict mode, and freeze
  decoded GRAY F32 fixtures rather than comparing container paths alone.
- Unlicensed MUF code cannot be redistributed. Mitigation: behavior-only
  specification, independent code, frozen results, and source inventory checks.
- CMake, Cargo, and npm increase packaging work. Mitigation: keep a process
  protocol instead of FFI, keep Rust limited to sidecar control, build the
  engine first, and let Tauri package one engine executable per platform.
- Metal and CUDA cannot both be proven on this host. Mitigation: require real
  Apple Silicon and NVIDIA CI lanes before enabling each backend by default.

## Verification Sequence

1. Unit tests for geometry, filters, banded algebra, thresholding, and grids.
2. CPU golden tests from frozen raw fixtures.
3. Protocol and media integration tests.
4. CPU benchmark and memory gates.
5. Metal conformance, benchmark, and memory gates on Apple Silicon.
6. Tauri React/Rust tests, screenshots, and real-worker end-to-end tests.
7. CUDA conformance, benchmark, and memory gates on NVIDIA hardware.
8. Packaging, binary dependency, license, and startup-fallback checks.

## Stop Conditions

- Do not begin GPU work until CPU oracle fixtures pass.
- Do not enable Metal or CUDA by default until correctness, valley, memory, and
  3x same-host performance gates pass.
- Do not ship code copied from `muvsfunc` without explicit permission.
- Do not claim CUDA verification without a real NVIDIA run.
- If the Metal MVP misses 3x after measured tuning, ship the optimized CPU
  backend first and revisit a parallel solver rather than weakening correctness.
