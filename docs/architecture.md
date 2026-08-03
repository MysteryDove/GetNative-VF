# Standalone GetNative Architecture

Status: accepted for implementation

## Objective

Build a standalone desktop application that reproduces the useful behavior of
GetFnative and `muvsfunc.getnative` without loading, importing, or requiring
VapourSynth, a VapourSynth plugin, Python, or `.vpy` scripts at runtime.

The product consists of two independently usable programs:

1. `getnative-engine`: a headless worker and CLI for decode, analysis, and
   export.
2. `getnative-gui`: a Tauri 2 desktop application that controls the worker and
   renders results.

The upstream repositories under `upstream/` are reference checkouts. They are
not product runtime dependencies and are excluded from the root repository.

## Source Of Truth

The behavior target is the current course implementation in
`upstream/muvsfunc/muvsfunc.py`:

- fractional geometry and parity handling: lines 5932-5982;
- descale/rescale argument flow: lines 6016-6065;
- error metric: lines 6092-6139;
- height, kernel, and frame modes: lines 6142-6467.

The mathematical inverse-resize reference is the standalone C core in
`upstream/descale/src/descale.c`:

- sampling weights: lines 247-285;
- Float32 forward/back substitution: lines 288-559;
- matrix construction and banded LDLT factorization: lines 562-647.

The old GetFnative repository remains a second compatibility target for its
letterbox, linear-light, candidate-grid, crop-10, and plotting behavior. It is
not the default profile.

## Hard Constraints

- No VapourSynth API, Python module, plugin loader, or `.vpy` execution in any
  release binary.
- One shared mathematical plan format for CPU, Metal, and CUDA.
- GPU execution must batch candidates and must not materialize every full
  reconstructed frame at once.
- CPU is the deterministic compatibility oracle and fallback.
- Metal and CUDA are optional runtime backends. Missing GPU support must not
  prevent the application from starting.
- Results must carry enough provenance to reproduce the run: source identity,
  frame timestamp/index, color interpretation, profile, candidate grid, kernel,
  geometry, metric, backend, device, engine version, and timing.
- Charts are generated from structured results. PNG byte identity across
  platforms is not required; data, valley ordering, axes, and visible style are.

## Decision

### Compute And Media: C++23

C++23 is the implementation language for the engine, coefficient planner,
CPU oracle, media pipeline, process protocol, and backend host adapters.

Reasons:

- CUDA is a C++ toolchain and Metal host integration is direct through
  Objective-C++.
- FFmpeg, zimg, and the MIT descale mathematical core expose C/C++ interfaces.
- The inverse operation is a banded triangular solve with strict operation
  ordering, so direct control of buffers, Float32 operations, and compiler flags
  matters more than a portable tensor abstraction.
- A stable C ABI can be added later without exposing STL types or allocator
  ownership.

### GUI: Tauri 2, Rust, React, And TypeScript

Tauri 2 is the GUI shell for macOS, Windows, and Linux. React/TypeScript renders
the workbench. A small Rust controller owns the lifecycle of
`getnative-engine`, forwards typed commands/events, and keeps the webview away
from arbitrary process execution. The Rust controller and engine communicate
over versioned JSON Lines on stdin/stdout. Bulk frame data never crosses JSON.

Reasons:

- The requested GUI/backend separation is a process boundary, so sharing the
  engine language with the UI provides little benefit.
- Tauri keeps the installer small while providing native windowing, dialogs,
  filesystem scopes, signing, and a Rust-owned sidecar boundary.
- React/TypeScript is well suited to a dense stateful analysis workbench and a
  custom SVG/Canvas plot, without coupling compute code to the webview.
- The verified host already has Node 22 and Rust/Cargo. Qt is not installed
  locally and its full formula pulls a substantially larger licensed module
  set.
- Tauri capabilities can restrict file and command access. The frontend never
  receives a general shell API.

### Rejected Primary Options

#### C++ And Qt Quick

Technically viable and the best fallback if a single build system becomes a
product requirement. It was not selected because the process-isolated backend
removes most single-language benefit, while Qt adds LGPL/GPL deployment review,
QML/C++ model plumbing, and a large platform packaging surface.

#### Flutter Desktop

Technically viable and already available on the host, but the user selected
Tauri. It remains a fallback if webview behavior becomes a product blocker.

#### Rust Compute Core

Memory-safe orchestration is useful in the Tauri shell, but CUDA kernels remain
CUDA C++, Metal still needs native bindings, and FFmpeg/zimg/descale remain FFI.
The numeric engine therefore stays C++ instead of duplicating its native
boundary inside Rust.

#### wgpu

Useful for portable graphics but not the primary compute layer. It does not
provide the explicit CUDA backend requested and makes backend-specific tuning
of banded forward/back substitution harder.

#### SwiftUI

Best for a macOS-only product, but it would force a second GUI for CUDA systems.

## Process Architecture

```text
React/TypeScript workbench
  | typed invoke/events
  v
Tauri Rust sidecar controller
  | JSONL commands/events over stdio
  v
getnative-engine worker / CLI
  |-- media: FFmpeg decode and stable frame indexing
  |-- color: explicit luma/GRAY F32 conversion
  |-- geometry: profile-specific candidate and crop parameters
  |-- planner: sparse A matrix + banded A^T A LDLT plans
  |-- scheduler: candidate grouping, tiling, cancellation, provenance
  |-- CPU backend: deterministic oracle and fallback
  |-- Metal backend: macOS Apple GPU
  `-- CUDA backend: NVIDIA driver backend
```

No local TCP server is used. The worker process provides crash isolation from
GPU drivers and media decoders, and the same executable supports automation.

## Planned Repository Layout

```text
app/                         Tauri 2 + React/TypeScript desktop GUI
  src/                       workbench, protocol types, plots, state
  src-tauri/                 Rust sidecar lifecycle and capability boundary
engine/
  CMakeLists.txt
  include/getnative/         stable engine types and protocol schema
  src/cli/                   CLI and JSONL worker
  src/media/                 FFmpeg decode, stream/frame selection
  src/color/                 luma and GRAY F32 conversion
  src/geometry/              compatibility profiles and candidate grids
  src/planner/               weights and banded LDLT coefficient plans
  src/backend/cpu/           deterministic scalar/SIMD executor
  src/backend/metal/         Objective-C++ host + .metal kernels
  src/backend/cuda/          CUDA driver host + .cu kernels
  src/export/                JSON, CSV, legacy TXT, SVG/PNG data model
  tests/                     unit, golden, integration, and benchmarks
protocol/                    versioned JSON schemas and examples
fixtures/                    redistributable synthetic golden inputs/results
docs/                        architecture, compatibility, and benchmark reports
```

## Compatibility Profiles

Profiles are explicit because the two references do not have identical
behavior.

### `muf-d278cd3` (default compatibility profile)

- Python round-to-even for derived base width.
- Python `int` truncation for fractional crop geometry.
- Repeated-addition candidate grid when strict legacy reproduction is selected.
- Default crop: 5 pixels on every edge.
- Default threshold: `0.015`, strict `difference > threshold`.
- Default metric: mean absolute retained error (`p = 1`).
- Adds `1e-9` only for log plotting, not to the stored raw metric.
- Height, kernel, and frame verification modes.

### `getfnative-44c8d0f`

- GetFnative letterbox and base-parity rules.
- Index-multiplication candidate grid.
- Default crop: 10 pixels on every edge.
- Optional linear-light preprocessing.
- Its fixed bicubic `(b=0, c=0.5)` search behavior.

### `modern`

- Decimal fixed-point candidate grids with no accumulated binary drift.
- Explicit color metadata and warnings when it must be inferred.
- Improved valley ranking and confidence fields.
- Never labels results as byte-identical to a legacy profile.

Candidate values are serialized as decimal strings together with
`grid_semantics`. Geometry uses explicit helpers for Python round-to-even and
truncation; C++ `std::round` is not compatible with Python at half ties.

## Standalone Numerical Pipeline

1. Decode the selected image or video frame with an LGPL-only FFmpeg build.
2. Resolve range, matrix, transfer, primaries, and chroma siting. Extract luma
   directly for YUV sources. Convert RGB to GRAY F32 with the selected profile.
3. Generate candidates with profile-specific decimal/grid semantics.
4. Compute integer native canvas size and fractional active-region offsets.
5. Build the forward resize matrix `A` in Float64, then build the banded
   `A^T A`, factor it with LDLT, and compress weights/factors to Float32 using
   the reference accumulation order.
6. Descale by calculating `A^T b`, forward substitution, diagonal scaling, and
   backward substitution for each independent row/column vector.
7. Reconstruct with the same stored forward matrix `A`; do not regenerate taps
   independently inside Metal or CUDA.
8. Apply absolute difference, strict threshold, metric crop, p-norm, and a
   deterministic reduction.
9. Return one structured metric and geometry record per candidate.

The CPU planner should construct `A^T A` directly in banded storage. The
reference implementation allocates dense intermediate matrices even though the
result is banded; eliminating that work is the first optimization and benefits
all execution backends.

## Metal And CUDA Batch Design

The optimized Metal backend follows this batch design:

- The CPU planner owns all Float64 coefficient generation. GPU kernels receive
  the same packed Float32 `AxisPlan` buffers.
- Candidates are grouped by output canvas dimensions, kernel support, and
  bandwidth, while retaining candidate-specific shift, active dimension, and
  coefficients.
- Horizontal solves map independent rows across lanes; vertical solves map
  independent columns. The solve direction itself remains ordered.
- Specialized bandwidth-3 and bandwidth-7 kernels cover bilinear and bicubic.
  A generic kernel covers Lanczos and spline modes.
- Forward reconstruction uses the retained `A` weights.
- The final forward pass fuses thresholding, crop checks, and block reduction.
  Full reconstructed frames are not written to host memory.
- The scheduler starts with 8-32 candidates per tile and adjusts to device
  memory. The source frame is uploaded once per job.
- GPU outputs fixed-order block partials. The CPU completes a Float64 merge in
  block-index order.
- `vertical_only` and width-only modes skip the unused axis.
- Strict mode disables fast math and uses fixed reduction order. Fast mode is
  opt-in and must be labeled in provenance.

The current Metal runtime reuses tiled command-buffer arenas backed by shared
`MTLBuffer`s, which fits unified-memory Apple Silicon and avoids redundant host
staging. Private-buffer staging remains a benchmark-driven future option.

CUDA was reset to an independent implementation and consumes immutable
`AxisPlan` values directly. The host packs forward weights tap-major, caches
resident plans per execution slot, uses pinned staging, and reuses grow-to-fit
device buffers. Candidate-local workspaces are bounded by a 640 MiB default,
and caller limits can only lower it; the explicit working set remains below
2 GiB. At context creation, the runtime snapshots free device memory, reserves
the larger of 512 MiB or one eighth of free memory (capped at half of currently
free memory), divides the remainder across execution slots, and leaves one
fifth of each slot budget outside the workspace. Retained device-buffer
capacities are included in the same per-slot and 2 GiB guards. Low-memory or
high-concurrency devices therefore use smaller candidate tiles instead of
blindly allocating 640 MiB per slot.

The CUDA dataflow is axis-specific:

- Horizontal input is transposed once so row-vector loads remain coalesced.
  The horizontal-only solve fuses F6 reconstruction and metric accumulation.
- Vertical-only solves advance two independent columns per thread. This reuses
  transpose and LDLT factors while keeping each warp's two column groups
  separately coalesced. The mixed/both path retains a single-column entry point
  so its lower register footprint is not contaminated by the paired kernel.
- Combined-axis horizontal solves use a shared-memory local transpose to emit
  row-major native data without a full-frame transpose pass. The final
  vertical-first reconstruction stages a narrow row span in shared memory and
  fuses thresholding and p=1 partial reduction.
- Device event telemetry separates staging, transfer, each kernel family,
  readback, cache hits, allocation counts, tile counts, and slot waits.

The host dynamically loads the CUDA Driver API, creates a dedicated context,
and embeds one nvcc-generated multi-architecture fatbin. The default CUDA 13.3
matrix contains native SM75 through SM121 targets and compute_75/compute_121
PTX fallbacks. Runtime compatibility has an independent SM75 floor; device
probing reports GPUs below that floor as incompatible. Compiler-generated PTX
is portability output, not a PTX optimization stage. Every configured cubin
and PTX entry is checked by CTest, and all native kernels must retain zero
stack and zero local-memory allocation.
CUDA promotion is strictly ordered:

1. The `cpp-generic` C++ path must pass real-device H/V/both conformance,
   tail-shape, cache, concurrency, tiling, and memory guards.
2. Shape-local C++ specialization is introduced one operation at a time and
   retained only with same-workload wall-time and counter evidence. These
   specializations remain internal to `cpp-generic`; the public
   `cpp-specialized` variant is still unavailable.
3. Inline or handwritten PTX may only be evaluated after this stage, with
   per-SM correctness and same-workload A/B evidence. Until then those runtime
   variants fail closed.

The RTX 5080 path has entered the profiling stage. Production promotion still
requires wider filter/geometry coverage and validation on additional CUDA
architectures. Current profile evidence is recorded in
`docs/performance/cuda-profile-20260802.md`; the SM75+ build, PTX-JIT, artifact,
and memory-budget evidence is recorded in
`docs/performance/cuda-portability-20260802.md`.

## Worker Protocol

Protocol v1 is JSON Lines with one object per line.

Commands:

- `hello`: negotiate protocol and engine version.
- `capabilities`: enumerate CPU/Metal/CUDA devices and supported profiles.
- `probe`: inspect media streams and frame metadata.
- `analyze`: submit a job whose candidates are decimal strings.
- `cancel`: request cooperative cancellation by job id.
- `export`: render JSON, CSV, legacy TXT, SVG, or PNG from a result file.
- `shutdown`: clean worker exit.

Events:

- `accepted`, `progress`, `warning`, `result`, `cancelled`, and `error`.

Every message contains `protocol_version` and a request or job id. Logs go to
stderr so stdout remains machine-readable.

## GUI Workbench

The first screen is the actual analysis workbench:

- file/frame queue with image/video drop, stream selection, frame number, and
  timestamp;
- compact segmented controls for Height, Kernel, Verify, and Batch;
- explicit min/max/step, base width/height and parity, H+W/H/W mode, border
  crop, metric crop, threshold, p-norm, kernel, and backend controls;
- stable interactive plot with log/linear mode, overlays, candidate tooltips,
  and no layout movement while progress updates;
- result table with valleys, metric, neighboring slope, crop geometry, and
  parity warnings;
- job queue with progress, cancellation, backend/device, tile size, and timing;
- JSON, CSV, legacy TXT, SVG, and PNG export.

The GUI must expose errors and fallback reasons. It must never silently switch
from strict to fast math or from GPU to CPU.

## Determinism And Equivalence

Bitwise equality between CPU, Metal, and CUDA is not a realistic product
contract because fused operations and parallel reductions can move pixels near
the strict `0.015` branch threshold. Equivalence is therefore defined at three
levels:

1. Geometry, candidate sequence, coefficients, and stored plan metadata are
   exact.
2. CPU strict output matches frozen reference fixtures within the initial
   bound `max(1e-7, 5e-4 * reference_metric)`; the bound is tightened after a
   representative corpus is measured.
3. GPU production output stays within the CPU bound, selects a minimum no farther
   than one search step, and preserves the top-k local-valley set.

The CPU compatibility backend remains available when a user requires the
closest legacy numeric result. Charts must contain the same data and visual
semantics (dark/light background, point-line series, log Y axis, labels, and
selected valleys); platform font rasterization may change PNG bytes.

## Performance Gates

Use a fixed benchmark fixture: GRAY F32 1920x1080, bicubic `(0, 0.5)`, 1000
fractional height candidates, crop 5, threshold 0.015.

- Planner optimization must use banded storage and stay below 512 MiB peak RSS.
- GPU execution must stay below 2 GiB working memory through candidate tiling.
- Metal on Apple Silicon must reach at least 3x the optimized CPU backend on the
  same host before the Metal backend is enabled by default.
- CUDA must reach at least 3x the optimized CPU backend on the same NVIDIA host
  before the CUDA backend is enabled by default.
- Cancellation must stop new tiles immediately and terminate within two tile
  durations.

No speedup claim is valid until it is measured on the same input, profile,
candidate set, compiler mode, and host.

## Licensing Boundary

- `upstream/descale` is MIT and its standalone mathematical core may be adapted
  with the copyright and license notice preserved.
- `upstream/GetFnative` is LGPL-2.1. Prefer behavior fixtures over source copy.
- `upstream/muvsfunc` has no repository license. Do not copy, vendor, rename,
  or redistribute its source without written permission.
- `upstream/zimg` is WTFPL. Keep it behind a replaceable color/resize adapter in
  case a downstream compliance policy rejects that license text.
- FFmpeg release builds must remain LGPL-only: no GPL or nonfree options, and
  notices/source-offer obligations must be documented.
- Tauri is MIT/Apache-2.0 and React is MIT. Every additional npm/Cargo package
  requires explicit license inventory before adoption.

## Current Verification Boundary

This host is an Apple M4 Max with a 40-core Metal 4 GPU and 128 GiB memory. It
has Node 22, Rust/Cargo, Xcode 27 beta, and the separately installed
MetalToolchain. Offline AIR/metallib compilation, embedded-library loading, and
runtime dispatch are verified on this machine.

The current Metal backend covers horizontal, vertical, and combined-axis
strict p=1 analysis. It retains specialized half-bandwidth-1/forward-width-2
and half-bandwidth-3/forward-width-4 pipelines, adds a generic path through
half-bandwidth 15 / forward width 16, and dispatches by actual plan shape.
Combined-axis execution uses fixed H-to-V inverse order, the same forward-order
heuristic as CPU, a shared bounded arena, and a fused final-axis metric. On the
declared 1920x1080 bicubic fixture with 1000 height candidates, the five-run
Metal median is 125.409 ms, the maximum CPU/Metal metric difference is 5.61e-8,
the valley distance is zero, peak arena workspace is 168.750 MiB, and all
explicit Metal buffers total 252.230 MiB at peak. Other p-norms, media input,
CUDA, and GUI analysis integration remain outside this proof; the GUI exposes
only the real geometry command until analysis exists.

`nvcc`, FFmpeg, zimg, pkg-config, and VapourSynth are not installed. CUDA
correctness and performance require a real NVIDIA CI/host and cannot be claimed
from this Mac.
