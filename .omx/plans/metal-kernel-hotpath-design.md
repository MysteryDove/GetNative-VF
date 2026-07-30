# Backend Hotpath Design Plan: Metal Kernels, CPU SIMD, and Planner Reuse

## 1. Status and requirements summary

- Workflow: direct single-owner planning. This is not a RALPLAN artifact and
  has no Architect/Critic consensus status.
- Source evidence: the controlled generic/fixed A/B, real-fixture profiling
  captured on 2026-07-30, and the current checkout on 2026-07-31.
- Coordination evidence: the current
  `.omx/plans/prd-pre-gpu-foundation-staged.md`. The staged plan
  supersedes the earlier all-phases draft and currently owns only Stage 0
  measurement plus Stage 1 bounded unique-plan construction
  (`.omx/plans/prd-pre-gpu-foundation-staged.md:3-21`).
- Goal: reduce strict backend latency across Metal execution, CPU vertical-axis
  solving, and AxisPlan construction for the complete supported filter family
  while preserving numerical behavior, bounded memory, result order, scalar/
  generic fallback, and architecture portability.
- Product boundary: backend-only. No Tauri/UI, protocol, CLI command, media
  decode, CUDA, or Vulkan feature work is included.

This plan does not modify, supersede, or approve the staged pre-GPU plan. Its
planner interventions begin only after that plan has frozen its Stage 0 and
conditional Stage 1 disposition; PL0/PL1 are skipped unless Stage 1 is retained
or a new evidence-backed planner plan explicitly authorizes them. Metal dispatch
consumes only stable facts already present in `AxisPlan`: exact
`half_bandwidth`, `forward_width`, coefficient buffers, and candidate order.

## 2. Current evidence

### 2.1 Current implementation

- The host accepts plans through half-bandwidth 15 and forward width 16, but
  dispatches only `(1,2)` and `(3,4)` to fixed-shape pipelines; every wider
  shape uses `generic` (`engine/src/backend/metal/metal_backend.mm:196-216`).
- Each shape owns five relevant pipeline stages: image inverse, final metric,
  matrix inverse, matrix forward, and horizontal-first final metric
  (`engine/src/backend/metal/metal_backend.mm:400-460`).
- The fixed inverse path removes the runtime band-width choice and preserves a
  shape-specific backward accumulation order
  (`engine/src/backend/metal/getnative.metal:47-171`).
- Matrix inverse, matrix forward, normal metric, and horizontal-first metric
  have fixed B3/B7 plus generic entry points
  (`engine/src/backend/metal/getnative.metal:174-303` and
  `engine/src/backend/metal/getnative.metal:305-441`).
- All coefficient math, including arbitrary Bicubic B/C, is performed by the
  CPU planner. Metal consumes packed Float32 plans, so kernel specialization is
  by numerical plan shape, not filter name (`engine/src/planner/filter.cpp:24-72`).
- The current conformance suite exercises every supported width through
  Lanczos8 in horizontal and vertical modes, plus both two-axis forward orders,
  but it does not force a same-shape generic-versus-specialized A/B
  (`engine/tests/metal_conformance_test.cpp:137-206` and
  `engine/tests/metal_conformance_test.cpp:290-340`).
- One source, workspace, and partial buffer are allocated per analysis call;
  nine plan buffers are allocated per tile
  (`engine/src/backend/metal/metal_backend.mm:553-565`,
  `engine/src/backend/metal/metal_backend.mm:622-644`, and
  `engine/src/backend/metal/metal_backend.mm:697-724`).
- The vertical CPU path invokes the scalar inverse once per image column
  (`engine/src/backend/cpu/cpu_analysis.cpp:383-392`). The solve then iterates
  destination row `i` inside each call and broadcasts no coefficient across
  adjacent columns (`engine/src/planner/axis_plan.cpp:549-588`). Adjacent
  columns are independent and contiguous in both the source and workspace, so
  they are a valid SIMD lane dimension without changing the dependency order
  inside a column.
- There is no current NEON, SSE, AVX, or portable SIMD implementation. The core
  target already disables floating-point contraction on Clang/GCC and uses
  `/fp:strict` on MSVC (`engine/CMakeLists.txt:98-115`).
- Both descale-matrix and zimg-forward construction first evaluate every tap to
  form a normalization sum and then evaluate the same taps again to emit
  normalized weights (`engine/src/planner/axis_plan.cpp:142-175` and
  `engine/src/planner/axis_plan.cpp:197-250`). This is exact duplicate work;
  Lanczos candidates therefore repeat the associated `sin` calls.
- Bicubic B/C changes weights and factor values, but geometry-derived topology
  is often identical across a sweep. It is not universally identical because
  descale construction removes exact zero weights
  (`engine/src/planner/axis_plan.cpp:167-192`). `B=0,C=0` is a known separate
  sparse case, and any other exact-zero-mask difference must also remain a
  separate topology.
- The current staged planner requires each worker to call the unchanged serial
  `build_axis_plan()` and evaluates Stage 1 using byte-identical plan vectors
  (`.omx/plans/prd-pre-gpu-foundation-staged.md:141-183`). Planner arithmetic
  changes in this plan therefore cannot be mixed into that baseline.

### 2.2 Shape map and priority

| Shape | Filter coverage | Current path | Priority |
| --- | --- | --- | --- |
| B3/F2 | Bilinear, Lanczos1 | fixed | retain |
| B7/F4 | Bicubic with any B/C, Spline16, Lanczos2 | fixed | retain |
| B11/F6 | Spline36, Lanczos3 | generic | P0 |
| B15/F8 | Spline64, Lanczos4 | generic | P0 |
| B19/F10 | Lanczos5 | generic | P1, lazy |
| B23/F12 | Lanczos6 | generic | P1, lazy |
| B27/F14 | Lanczos7 | generic | P1, lazy |
| B31/F16 | Lanczos8 | generic | P1, lazy; profile first after P0 |

`B<n>` is total LDLT band width. The corresponding stored half-bandwidth is
`(n - 1) / 2`; `F<n>` is the forward reconstruction width.

### 2.3 Performance evidence and proof boundary

The controlled A/B routed the same metallib and plans either to the existing
fixed or generic pipelines:

| Shape | Fixed median | Generic median | Latency reduction |
| --- | ---: | ---: | ---: |
| B3/F2 | 95.694 ms | 109.091 ms | 12.3% |
| B7/F4 | 110.235 ms | 125.569 ms | 12.2% |

The same session measured the current generic B11/F6 Spline36 path at
164.729 ms and B15/F8 Spline64 at 207.264 ms under its 1920x1080,
1000-candidate vertical setup. These numbers establish direction, not a final
gate: they used five samples and predate any subsequent source change.

The real `6.2-1.png` profile found that inverse plus metric GPU work dominates,
while command encoding is small. It also found a large sustained Lanczos8
slowdown that did not reproduce on an interleaved Bicubic sentinel. Therefore:

1. Fixed band and forward widths are the first kernel intervention.
2. Pipeline/encoder fusion is not an early target.
3. Lanczos8 requires both fresh and sustained evidence.
4. Buffer reuse is a host-runtime optimization and must be measured separately
   from kernel specialization.

## 3. Design decisions

### 3.1 Dispatch by exact shape, never by filter name

Introduce a Metal-private shape key:

```text
MetalKernelShape {
  half_bandwidth,
  forward_width,
  backward_accumulation_order
}
```

The filter type, Bicubic B/C values, Lanczos tap label, candidate id, and
planner/cache identity do not participate in pipeline selection. Exact
`half_bandwidth` and `forward_width` already exist in every descriptor
(`engine/src/backend/metal/metal_backend.mm:37-54`).

The generic path remains mandatory for unsupported, disabled, or rejected
specializations. A benchmark-only diagnostic policy may force `generic_only`;
normal product dispatch remains automatic.

### 3.2 Preserve strict arithmetic order

- B11 through B31 must initially use the current generic far-to-near backward
  accumulation order.
- B7 retains its existing near-to-far descale-compatible order.
- Specialization may replace runtime loop bounds with constants and permit
  unrolling, but must not reorder multiply-add terms deliberately.
- Strict Metal compilation remains `-fno-fast-math -ffp-contract=off`
  (`engine/CMakeLists.txt:58-66`).

### 3.3 Two-stage specialization strategy

1. Add static fixed-shape entry points for B11/F6 and B15/F8 using the existing
   helper/macro pattern. Create those named pipelines lazily per stage. This is
   the shortest proof that wider shapes benefit without introducing a new
   runtime specialization mechanism or eager construction of ten pipeline
   states.
2. Add function-constant specialization for B19/F10 through B31/F16. Create
   pipeline states lazily per shape and per stage so a vertical-only Lanczos5
   scan does not pay for unused two-axis stages.

Keep B3/B7 as the known-good static controls. Do not migrate them to function
constants in the same change; that would remove the stable control needed to
attribute regressions.

### 3.4 Persistent Metal arena and bounded upload ring

The current workspace buffer is already reused across every tile in one call
(`engine/src/backend/metal/metal_backend.mm:624-646`). The missing reuse is
across analysis calls and, most visibly, the nine new shared plan buffers per
tile (`engine/src/backend/metal/metal_backend.mm:697-724`). After the staged
planner has retained its Stage 1 evidence, evaluate:

- grow-to-fit engine-owned shared buffers for source, workspace, and partials;
- one aligned plan-upload `MTLBuffer` per ring slot containing descriptors and
  all eight coefficient/index tables;
- binding the same arena buffer at the existing Metal buffer indices with
  checked table offsets, so this does not require an argument-buffer redesign;
- a two-slot baseline and one four-slot experiment; a one-slot ring is rejected
  because it forces a wait before every tile can overwrite the only slot;
- completion-aware reuse: a slot remains in flight until every command buffer
  referencing it has completed, including cancellation and failure paths;
- separate active-byte, retained-capacity, allocation-count, wiring-time, and
  high-water telemetry with explicit benchmark reset.

This spends a bounded amount of memory to remove allocation/wiring and permit
CPU packing of a later tile while the GPU consumes an earlier tile. It does not
overlap live regions within a slot, use `MTLHeap` initially, or wrap arbitrary
decoder memory with `newBufferWithBytesNoCopy`.

### 3.5 Packed-plan content cache is distinct from buffer reuse

The upload ring reuses allocations but still repacks and uploads content. A
second, independently removable experiment caches immutable topology,
coefficient, and factor blocks across repeated analyses:

```text
MetalPackedPlanKey {
  packing_abi_version,
  device_registry_id,
  exact_axis_plan_identity_or_verified_content_digest,
  coefficient_representation
}
```

- Pointer addresses alone are forbidden as identity. Prefer the staged
  planner's final canonical key if it becomes a stable contract; otherwise use
  an exact content digest followed by size and byte equality before reuse.
- Cache only immutable plan payload. Per-dispatch descriptors, workspace bases,
  candidate indexes, job fields, and partial-result offsets remain transient.
- Store cached payload in a bounded engine-owned aligned Metal arena. Descriptors
  refer to its table bases; generic and specialized kernels consume the same
  payload layout.
- Bound by both bytes and entry count, use completion-aware LRU reclamation, and
  purge on device change, packing-ABI change, explicit engine reset, or memory
  pressure. An in-flight allocation cannot be reclaimed.
- Measure cold miss, warm hit, bytes hashed, bytes packed, bytes uploaded,
  eviction count, and retained bytes. Keep/remove this cache independently from
  the upload ring; a cheap ring must not justify an unprofitable content cache.

This is an engine/session cache, not an unbounded process-global or disk cache.
It starts only after Stage 1 and a serialized identity/packing checkpoint,
because the current staged plan deliberately does not freeze a portable packing
ABI (`.omx/plans/prd-pre-gpu-foundation-staged.md:167-171`).

### 3.6 CPU adjacent-column SIMD, AArch64 first

Add an internal batched-column inverse interface used by the vertical CPU path.
Keep the current public scalar `inverse_axis_f32()` as the oracle and fallback.
The first implementation is Apple Silicon/AArch64 NEON:

- make destination row `i` the outer dependency loop and map adjacent `x`
  columns to four Float32 NEON lanes;
- broadcast each transpose weight, lower/upper factor, and inverse diagonal;
  load/store adjacent source and workspace columns contiguously;
- issue explicit multiply followed by add/subtract, never an intrinsic fused
  multiply-add, while the strict build keeps contraction disabled;
- retain far-to-near forward/backward factor order for generic widths and the
  existing B7 near-to-far backward exception
  (`engine/src/planner/axis_plan.cpp:563-587`);
- handle non-multiple-of-four widths with the scalar oracle, and feed any metric
  reduction in original scalar pixel order.

Use a narrow internal dispatch boundary such as `inverse_columns_f32()` with
scalar and NEON translation units. On AArch64 NEON is mandatory, so compile-time
selection is sufficient. Preserve the same boundary for a later x86-64
translation unit with runtime CPUID selection among scalar, SSE2, and AVX2;
do not compile x86 intrinsics into the ARM target or claim x86 performance from
Apple Silicon. AVX-512 is a separate evidence-based option, not a requirement.

The first SIMD milestone covers the vertical inverse solve only. Horizontal-row
batching and vectorized forward reconstruction are separate follow-ups because
they have different data layout and metric-reduction risks.

### 3.7 Reuse exact tap weights and consume Stage 1 parallelism

Within each descale or zimg-forward row, evaluate each raw tap weight once into
the existing bounded row scratch, sum those stored values in the current tap
order, then normalize the same stored values in the current emission order.
This removes the second filter evaluation without approximation, reassociation,
or a new cache lifetime.

Across candidates, use only the staged planner's bounded unique-plan batch API.
Every `build_axis_plan()` remains serial; no nested pool, intra-plan parallel
region, or planner/GPU overlap is introduced here. This intervention begins
after Stage 1 so its before/after result cannot contaminate the parallel-planner
baseline.

### 3.8 Bicubic B/C sweeps share only verified topology

Split Bicubic sweep data conceptually into:

- topology: transpose offsets/indices, forward offsets/left indices, dimensions,
  support, and exact zero mask;
- values: transpose/forward weights, lower/upper factors, and inverse diagonal.

Generate geometry once per batch-compatible request family, but derive an exact
topology fingerprint for each B/C result after border mapping, duplicate
coalescing, and zero filtering. Intern topology only when fingerprint, lengths,
and bytes all match. B/C values remain exact-bit key fields; there is no epsilon
grouping. `B=0,C=0` always takes an independent sparse path, and any other B/C
pair with a different exact-zero mask does too.

First reuse the geometry skeleton and intern identical blocks inside the
batch/Metal packed-plan layer without changing `AxisPlan`'s public flat vectors.
An optional shared `AxisPlanTopology` representation is allowed only after a
separate profile proves retained host-plan memory is material and all CPU/Metal
consumers have a serialized migration checkpoint.

### 3.9 Low precision remains an optional fast-mode experiment

The only permitted first experiment is Float16 coefficient storage with
Float32 loads/accumulation, workspace, partials, and final merge. Never store
the triangular solve workspace or inverse diagonal accumulation in Float16.
Strict mode remains all-Float32 even if the experiment succeeds.

## 4. Coordination boundary

### 4.1 Not owned by this plan

- Stage 0 timer definitions and Stage 1 request normalization, unique-key batch
  scheduling, worker caps, stable scatter, and acceptance decision.
- A persistent planner service, cross-call single-flight, production scheduler,
  public scan error ABI, or portable CUDA/Vulkan packing contract.
- General CPU candidate-pool policy or overlap between planning and execution.
- Tauri/UI and public worker protocol.

Stage 0/1 stays with `.omx/plans/prd-pre-gpu-foundation-staged.md` and
`.omx/plans/test-spec-pre-gpu-foundation-stage-1.md`. Neither document is edited
by this plan. Post-Stage-1 exact tap reuse, B/C topology experiments, CPU SIMD,
Metal specialization, and Metal-private bounded packed-plan reuse are owned here.

### 4.2 Integration rule

- Freeze the staged Stage 0 baseline and finish the conditional Stage 1 decision
  on an
  unchanged metallib, Metal allocation path, scalar CPU path, and serial
  `build_axis_plan()` implementation. No hotpath result may become one side of
  the Stage 1 A/B.
- Metal shader work, the new dedicated kernel benchmark, and new CPU SIMD source
  files may be developed in isolation, but their CMake/benchmark integration is
  merged only after the Stage 1 evidence commit.
- PL0/PL1 are strictly after a retained Stage 1 because they modify or
  reinterpret `axis_plan.cpp` output construction. They consume the accepted
  batch planner and never introduce a competing pool. If the staged program
  stops after Stage 0 or removes Stage 1, record both as
  `SKIPPED_NOT_JUSTIFIED` here.
- MK3 starts after the staged worktree has a terminal evidence commit and MK0
  has measured host pack/upload share. Allocation-only
  arena/ring reuse may proceed with the current private layout; packed content
  caching additionally requires a serialized plan-identity and packing-ABI
  checkpoint.
- Keep specialized/generic dispatch Metal-private. If a later portable layout
  groups multiple numerical shapes under one signature, Metal locally splits
  them before selecting a pipeline; no Metal pipeline enum enters the planner.
- MK0 may initially report complete metric vectors and argmin using current
  behavior, but every keep decision uses one shared local-valley oracle after
  that helper is available. This plan does not create a second interpretation
  of top-five valleys.

### 4.3 Worktree isolation and final integration

The repository currently has an unborn `main`, no valid `HEAD`, and no tracked
files. Actual execution therefore starts by preserving the current tree and
creating one reviewed common baseline commit; `git worktree add` cannot provide
the requested isolation before that commit exists.

Create three sibling worktrees from that exact baseline:

| Worktree/branch | Ownership before integration | Forbidden before checkpoint |
| --- | --- | --- |
| `pre-gpu-stage1` / `perf/pre-gpu-stage1` | staged Stage 0/1 planner, focused tests, existing benchmark timer wiring | Metal kernels/runtime, CPU SIMD, PL0/PL1 |
| `backend-hotpath` / `perf/backend-hotpath` | `getnative.metal`, Metal-private runtime, dedicated MK0 benchmark, CPU SIMD sources/tests | staged plan artifacts and `axis_plan.cpp` until Stage 1 is accepted |
| `perf-integration` / `integration/perf-stack` | merge-only conflict resolution, combined benchmarks, final evidence | independent feature development |

Rules:

1. Use separate build and artifact directories per worktree. Never share a CMake
   cache, generated metallib directory, benchmark JSON, or profiler capture.
   Copy the ignored local `6.2-1.png` into each worktree and pass its committed
   SHA-256 check before profiling.
2. Each branch records its baseline commit, compiler/binary/metallib hashes,
   owned-path inventory, and exact benchmark commands. Unexpected edits outside
   ownership stop that branch; they are not reset or overwritten.
3. Finish and validate `perf/pre-gpu-stage1` first. Merge its Stage 0/conditional
   Stage 1 terminal commit into the integration worktree and rerun the applicable
   staged gate before accepting any hotpath result.
4. Merge the independent Metal/CPU hotpath commits next. Resolve shared CMake or
   existing-benchmark edits only in the integration worktree, then rerun both
   branches' gates. Prefer the dedicated MK0 benchmark so benchmark conflicts
   remain small.
5. Only after this checkpoint may the backend worktree advance from the accepted
   integration commit and, if Stage 1 was retained, implement PL0/PL1. The
   identity-dependent portion of MK3 additionally waits for its own key/packing
   checkpoint. The pre-GPU branch is frozen; it is never edited to absorb these
   changes.
6. Final integration is a normal reviewed merge into `integration/perf-stack`,
   followed by the complete verification matrix. Preserve both branch histories;
   do not use reset, force checkout, or conflict resolution that discards either
   side.

This arrangement has no file-level interaction with Tauri/UI planning. It does
interact with pre-GPU planner parallelization at `axis_plan.cpp`, CMake, and the
benchmark baselines, which is why those points are explicit serialized merge
gates rather than concurrent shared-worktree edits.

## 5. Milestones

Execution order is: retain the staged Stage 0/conditional Stage 1 terminal
commit; run MK0; evaluate MK1/MK2 and CPU0 independently; run PL0 then PL1 only
on top of a retained Stage 1 planner; run MK3 after host pack/upload evidence;
leave MK4 last. Each milestone produces a separate commit and terminal `KEPT`,
`REMOVED_AFTER_TEST`, or `SKIPPED_NOT_JUSTIFIED` disposition before the next
dependent milestone.

### MK0 - Freeze a kernel-specific A/B benchmark

Files:

- New `engine/bench/metal_kernel_benchmark.mm`
- New `engine/bench/fixtures/metal_kernel_matrix.json`
- Existing local-only `engine/bench/fixtures/6.2-1.png` plus its committed
  checksum and fixture manifest
- `engine/CMakeLists.txt` for a benchmark-only ImageIO/CoreGraphics target
- Minimal diagnostic dispatch control in the Metal backend/options

Work:

1. Verify the local fixture against `6.2-1.png.sha256`, load it through a
   benchmark-only macOS image decoder, and convert it deterministically to
   Float32 luma. The media file remains ignored until redistribution rights are
   established; each worktree uses an independently copied, hash-identical file.
2. Support Bilinear, arbitrary Bicubic B/C, Spline16/36/64, and Lanczos1-8.
3. Support `automatic`, `generic_only`, and `required_specialized` dispatch;
   report the selected shape and every created pipeline stage.
4. Run 1000-candidate fractional scans at native heights 362, 540, 720, 810,
   846, 864, and 900, plus the existing `800..899.9` kernel-scan case.
5. Record CPU wall, backend wall, GPU stage time, pipeline creation time,
   allocation count/bytes, peak active/retained bytes, metric error, argmin,
   and, once the shared oracle lands, top-five local valleys. Record the
   decoded Float32 input SHA-256 so decoder drift cannot masquerade as a kernel
   change.
6. Use at least 21 alternating same-process generic/specialized pairs for a
   performance decision. Reject evidence when paired-ratio MAD exceeds 0.02 or
   the build/metallib hash changes.
7. Add a sustained sequence for Lanczos4 through Lanczos8 with an interleaved
   B7 sentinel. Record first- and last-decile medians and macOS thermal state;
   a thermal-state transition invalidates the comparison.

Exit gate:

- Forced generic and automatic modes use identical plans and inputs.
- Current B3/F2 and B7/F4 controls reproduce a statistically significant win
  in the same direction as the historical A/B.
- The fixture, exact parameters, raw samples, binary hash, metallib hash, and
  result vectors are retained in the report.

### MK1 - Add fixed B11/F6 and B15/F8 kernel families

Files:

- `engine/src/backend/metal/getnative.metal`
- Metal-private shape/pipeline selection code
- New `engine/tests/metal_kernel_specialization_test.cpp`
- The MK0 benchmark

Work:

1. Add fixed entry points for all five stages for B11/F6 and B15/F8, retaining
   the embedded library and constructing only the stages first requested by a
   workload.
2. Extend automatic dispatch and tile signatures to distinguish these exact
   shapes without changing common planner identity.
3. Assert `required_specialized` fails loudly if any required stage silently
   falls back to generic.
4. Cover Spline36/Lanczos3 and Spline64/Lanczos4 in vertical, horizontal,
   two-axis vertical-first, and two-axis horizontal-first paths.

Keep gate, evaluated per shape:

- Primary 1920x1080 fixture at the 810-height scan has
  `median((specialized - generic) / generic) <= -0.05` for both filters sharing
  the shape.
- No named resolution or axis mode regresses by more than 3% median.
- CPU/Metal tolerance, argmin distance, top-five valley matching, result order,
  cancellation, and `<2 GiB` absolute memory gates remain green.
- Pipeline creation and metallib size are reported, not hidden in steady-state
  timing.

If one shape fails, remove only that shape's entry points and dispatch. The
other shape is decided independently.

### MK2 - Add lazy function-constant specialization for Lanczos5-8

Files:

- `engine/src/backend/metal/getnative.metal`
- New Metal-private pipeline repository implementation, if extraction reduces
  rather than increases host-backend complexity
- The MK0 benchmark and specialization tests

Work:

1. Extend the MK1 lazy pipeline repository to create function-constant
   per-stage states for B19/F10, B23/F12, B27/F14, and B31/F16.
2. Key the cache by exact shape, stage, strict/fast mode, and accumulation-order
   contract. The existing serialized engine call makes initial cache admission
   single-owner; do not add another worker pool.
3. Build only inverse plus metric for a single-axis request. Materialize matrix
   inverse/forward/horizontal-first metric only when a two-axis path first uses
   them.
4. Keep generic fallback and record specialization miss/failure telemetry.
5. Decide every Lanczos shape independently; an unprofitable shape stays
   generic without disabling the others.

Keep gate per shape:

- Fresh paired full-backend median improves at least 5% on the 810-height and
  `800..899.9` cases.
- No resolution or axis mode regresses by more than 3%.
- Lanczos8 sustained median improves at least 5% versus forced generic, the
  last-decile median is not worse than generic, and the B7 sentinel remains
  within 3%.
- No correctness, ordering, cancellation, memory, or validation gate regresses.

Implementation order is B31 first after the common mechanism, then B19, B23,
and B27. The mechanism is shared, but each pipeline shape retains an explicit
keep/remove result.

### CPU0 - Add AArch64 adjacent-column SIMD for the vertical inverse

Files:

- `engine/src/backend/cpu/cpu_analysis.cpp`
- New CPU-private batched-column dispatch plus scalar and NEON translation units
- `engine/CMakeLists.txt`
- New focused CPU SIMD correctness/performance test and benchmark support

Work:

1. Add `automatic`, `scalar_only`, and `required_simd` diagnostic dispatch for
   the internal vertical inverse; public CPU results and APIs remain unchanged.
2. Implement four-column NEON lanes with `i` outermost, coefficient broadcasts,
   explicit non-fused multiply/add-subtract operations, and the exact B7 versus
   generic backward orders. Keep a scalar width tail.
3. Do not vectorize the final metric reduction in this milestone. Reconstruction
   values may be stored in vector batches, but `MetricAccumulator::add()` remains
   in original x/y order.
4. Test B3/F2 through B31/F16, odd widths, widths below four, nontrivial strides,
   all border modes, custom B/C, subnormal/near-zero inputs, and both full and
   cropped metrics against `scalar_only` bit-for-bit.
5. Profile 1920x1080 and the 362/540/720/810/846/864/900 plus `800..899.9`
   vertical cases on Apple Silicon. Use 21 alternating scalar/NEON pairs and
   record inverse-stage and whole-candidate wall separately.

Keep gate:

- Every strict CPU metric and materialized inverse workspace element is
  bit-identical to scalar, including tails and the B7 accumulation exception.
- Whole vertical-candidate median improves at least 5% on the 1080-to-810
  primary case, and no named filter/resolution regresses more than 3%.
- Binary/compiler hashes and the selected ISA path are recorded. A non-ARM host
  must report scalar fallback, not silently label scalar code as SIMD.

If the gate fails, remove NEON dispatch and retain the internal batched boundary
only if it simplifies neither public API nor scalar execution. The x86 path is
not implemented or claimed in CPU0.

### PL0 - Eliminate duplicate tap evaluation after Stage 1

Files:

- `engine/src/planner/axis_plan.cpp`
- Core/upstream planner tests
- Stage 1 planner benchmark and MK0 reporting

Work:

1. Store raw descale tap weights once per row, sum in the current tap order, and
   reuse those exact doubles during border mapping, zero testing, coalescing, and
   normalization.
2. Reuse the existing zimg `tap_weights` scratch in the same way: populate once,
   form `total` from that array in current order, then normalize without a second
   `kernel.weight()` call.
3. Run through the accepted Stage 1 `build_axis_plans()` path for multi-candidate
   scans. Do not add a pool or parallelize work inside one plan.
4. Profile plan-stage wall and `sin`/filter-weight hotspots for Bilinear,
   arbitrary Bicubic B/C, Spline16/36/64, and Lanczos1-8 over the complete MK0
   resolution/candidate matrix.

Keep gate:

- Every `AxisPlan` field/vector is byte-identical to the accepted Stage 1 serial
  oracle for all filters, shifts, active lengths, and border modes.
- Lanczos3-8 1000-candidate plan-stage median improves at least 5%, sampled
  `sin`/weight work declines consistently, and no non-Lanczos planner case
  regresses more than 3%.
- Scratch remains bounded by the row's tap count and adds no persistent cache or
  cross-call lifetime.

### PL1 - Share exact Bicubic sweep topology

Prerequisite: PL0 has a terminal disposition and the accepted Stage 1 key
contract is available.

Files:

- Planner-private batch/topology helper and focused tests
- Metal packed-plan builder/cache when MK3 content caching is enabled
- Planner and MK0 benchmarks

Work:

1. Freeze a B/C sweep containing at least Mitchell `(1/3,1/3)`, Catmull-Rom
   `(0,1/2)`, B-spline `(1,0)`, `(0,0)`, additional supported sharp/soft values,
   and exact duplicate requests at the named resolutions and fractional shifts.
2. Build a geometry skeleton once per compatible source/destination/active-
   length/shift/support/border family, then compute each variant's exact zero
   mask and topology fingerprint.
3. Intern offsets/indices/forward-left blocks only after length and byte equality;
   keep weights and LDLT factors private to each B/C plan. Track topology hit,
   miss, unique-block count, retained bytes, and equality-check bytes.
4. Start with batch-local geometry reuse and Metal packed-block interning. Do not
   change public `AxisPlan` storage unless retained host-plan memory is measured
   as material and a separate migration sub-gate is approved.

Keep gates, decided independently:

- Geometry reuse: plan bytes remain identical and B/C sweep plan-stage median
  improves at least 5%, with no single-value regression above 3%.
- Packed topology interning: warm packed/upload topology bytes fall at least 80%
  for equality groups and total explicit working set falls at least 10% or Metal
  total wall improves at least 3%, with no case above 3% regression.
- Any shared host representation: retain only if sweep retained-plan bytes fall
  at least 20%, CPU/Metal execution remains within 3%, and all consumers pass.

`B=0,C=0` and every other differing zero mask must produce a distinct topology;
one false-sharing result removes the affected sharing layer.

### MK3 - Add persistent Metal arenas, upload ring, and packed-plan cache

Prerequisites: the staged worktree has a terminal Stage 0/conditional Stage 1
evidence commit. The content-cache substage also requires a stable exact plan
identity and private packing-ABI version; if host pack/upload is below 10% of
Metal total and repeated reuse is not a target workload, record
`SKIPPED_NOT_JUSTIFIED` for content cache.

Files:

- Metal backend runtime and private buffer-pool implementation
- MK0 benchmark
- Metal cancellation/conformance tests

Work:

1. Add grow-to-fit reusable source, workspace, and partial buffers across calls
   with checked capacity and a configured retained-byte ceiling. Do not claim
   within-call workspace reuse as new work; it already exists.
2. Pack/bind descriptors and plan tables through one aligned shared arena per
   ring slot rather than nine buffers per tile.
3. Evaluate exactly two ring depths: 2 and 4. Reuse a slot only after its
   consuming command completes; cancellation drains submitted work and leaves
   every slot reusable.
4. Add the bounded immutable packed-plan content cache described in section 3.5
   as a separate switch. Keep descriptors/workspace offsets transient and make
   eviction completion-aware.
5. Report active bytes separately from retained capacity, cache residency, and
   bytes packed/uploaded; reset high-water
   telemetry explicitly between benchmark cases.
6. Keep the current allocation path and uncached upload path behind diagnostic
   comparisons until
   conformance, cancellation, and paired performance evidence pass.

Keep gate:

- Repeated-scan Metal buffer allocation calls fall by at least 80% and directly
  captured allocation/wiring time falls by at least 50%; neither value may be
  inferred from byte counts.
- Repeated-scan backend wall improves at least 3% on one primary workload and
  no workload regresses by more than 3%.
- The selected ring depth adds no more than 5% to the prior explicit working-set
  peak before content-cache residency. Total active plus retained cache remains
  below 2 GiB and every configured ceiling.
- No CPU/GPU write-after-read hazard appears under Metal API/Shader Validation;
  cancellation and immediate engine reuse pass.
- On a repeated identical-plan workload, a warm packed-content cache reduces
  bytes repacked and uploaded by at least 80% and improves Metal total wall at
  least 3%, with no cold or eviction-heavy workload above 3% regression. If it
  misses, remove only content caching and retain any independently passing arena.

If neither ring depth passes, retain only safe reusable source/workspace/partial
buffers that independently pass the same gates; remove the plan ring. Arena,
ring depth, packed content cache, and B/C topology interning each receive their
own disposition.

### MK4 - Optional Float16 coefficient-storage experiment

Prerequisites: MK1-MK3 and CPU0/PL0/PL1 decisions are final and strict Float32
is the retained baseline.

Work:

1. Add an explicit non-default fast-mode representation for coefficient arrays
   only. Indices, descriptors, workspace, accumulators, partials, and CPU merge
   stay 32-bit.
2. Run the full resolution/filter corpus, the sustained Lanczos8 case, extreme
   Bicubic B/C values supported by the public API, and near-threshold fixtures.
3. Report metric drift distribution, not only the maximum, and require exact
   argmin/top-five valley identity for every retained fixture.

Keep gate:

- Strict mode remains byte- and behavior-compatible with the retained Float32
  path.
- Fast mode improves Lanczos8 sustained median by at least 8%, reduces the
  coefficient-array payload to at most 52% of the Float32 payload after
  alignment, and does not raise total working set.
- Every corpus valley/argmin gate passes. Otherwise remove the fast-mode path.

Float16 triangular workspace is explicitly outside this plan.

## 6. Deferred or rejected interventions

- Changing tile size, reduction group count, or inverse threadgroup width:
  current profiling found the existing 32/8/32 defaults locally best.
- More command-buffer fusion or indirect command buffers: encoder time was a
  small fraction of the observed trace.
- GPU final merge: only a small fixed partial array is merged on CPU
  (`engine/src/backend/metal/metal_backend.mm:972-978`).
- SIMD-group reduction in strict mode: it changes reduction order. Reconsider
  only for fast mode if metric remains at least 35% of backend wall after MK2.
- Fusing inverse and metric or removing the Float32 workspace: high-risk
  algorithm redesign with different dependency and reduction structure.
- Intra-AxisPlan thread parallelism: LDLT and each column solve contain ordered
  dependencies. CPU0 vectorizes independent columns and does not relax this.
- x86 SIMD implementation: preserve the internal dispatch boundary now, but add
  SSE2/AVX2 only after a real x86-64 profile and strict-bit comparison. Apple
  Silicon numbers are not evidence for vector width or dispatch policy on x86.
- Horizontal-row SIMD and vectorized metric reduction: evaluate only after CPU0
  proves vertical inverse benefit; strict metric reduction order stays scalar.
- ANE/AMX integration: no suitable public arbitrary-kernel execution contract
  for this ordered banded solve.

## 7. Risks and mitigations

| Risk | Mitigation and stop condition |
| --- | --- |
| Wider unrolling raises register pressure or lowers occupancy | Decide each shape independently from paired backend wall and GPU-stage evidence; remove a shape that misses its 5% keep gate |
| A fixed kernel changes strict solve order | Keep B11-B31 on the generic far-to-near order, retain strict compiler flags, and compare forced generic against specialized result vectors |
| More pipeline states increase engine startup latency | Create new shapes and stages lazily, time cold creation separately, and never include it only in one side of a steady-state A/B |
| Function-constant creation fails on a supported device | Keep generic fallback with explicit telemetry; `required_specialized` makes the failure visible in tests and profiling |
| A reusable arena is overwritten while the GPU still reads it | Reuse a ring slot only after its consuming command completes; run API/Shader Validation plus cancellation-and-immediate-reuse stress |
| Grow-only buffers hide retained-memory growth | Enforce a configured retained-byte ceiling and report active bytes separately from retained capacity; reject any path over the absolute 2 GiB gate |
| Packed-plan identity reuses stale or unequal content | Version the packing ABI and device key, verify digest hits with sizes and exact bytes, forbid pointer-only identity, and reclaim only after GPU completion |
| NEON changes strict values through contraction, FTZ, or lane reduction | Use explicit non-fused operations, preserve per-lane term order, avoid horizontal reduction, and require bit-identical workspace/results including subnormal fixtures |
| Tap reuse changes a zero mask or rounding | Sum stored raw doubles in the original order and require every plan vector byte-identical before accepting any timing result |
| B/C topology is assumed equal when an exact weight is zero | Fingerprint after mapping/coalescing/zero filtering, verify bytes, force `(0,0)` separate, and remove sharing on one false hit |
| Lanczos8 results are distorted by thermal throttling | Use a B7 sentinel, first/last deciles, and thermal-state capture; invalidate rather than reinterpret a run with a thermal-state transition |
| Float16 changes valleys near the metric threshold | Keep Float32 strict mode, require exact corpus argmin/top-five identity in fast mode, and remove the experiment on any failed fixture |
| Parallel worktrees contaminate a baseline or overwrite shared files | Start from one committed tree, use disjoint owned paths/build dirs, merge Stage 1 first in the integration worktree, and rerun both gates after every conflict resolution |

## 8. Verification matrix

| Claim | Required proof |
| --- | --- |
| Correct dispatch | forced generic/required specialized tests for every exact shape and stage |
| Numerical safety | CPU vs generic vs specialized result vectors; strict tolerance; argmin and top-five valley gates |
| Full mode coverage | H, V, both forward orders, mixed adjacent shapes, custom Bicubic B/C |
| Performance | 21 alternating paired samples, median/p95/MAD, unchanged binary/metallib hashes |
| Sustained behavior | L4-L8 sequence, B7 sentinel, stable thermal state, first/last deciles |
| Resource safety | allocation/wiring counts, active and retained bytes, absolute/configured ceilings |
| Synchronization | Metal API and Shader Validation, ring-slot reuse stress, cancellation then immediate reuse |
| Fallback | generic-only produces conformant results when a specialization is disabled or removed |
| CPU SIMD correctness | scalar-only versus required-NEON workspace and final metrics, bit-for-bit, across all shapes, tails, strides, borders, and strict FP edge cases |
| CPU SIMD performance | 21 alternating pairs on named heights, inverse-stage plus whole-candidate wall, selected ISA and binary hash |
| Planner tap reuse | byte-identical AxisPlans plus paired plan-stage timing and `sin`/weight hotspot evidence |
| B/C topology safety | exact topology byte oracle, zero-mask separation, unique-block counts, memory and execution deltas |
| Packed content cache | cold/miss/warm/eviction cases, hash/equality cost, packed/upload bytes, completion-aware reclamation, bounded residency |
| Worktree integration | common baseline and branch commit IDs, owned-path audit, independent branch gates, then combined integration gates |

Run the existing core, upstream, and Metal conformance targets after every
retained milestone. A compile-only or skipped-no-device result is not Metal
runtime evidence.

## 9. Overall acceptance criteria

This plan is complete only when:

1. B11/F6 and B15/F8 each have a recorded keep/remove decision.
2. B19/F10 through B31/F16 each have a recorded lazy-specialization decision.
3. Generic fallback remains tested for every supported shape.
4. CPU0 has a terminal scalar-versus-NEON decision on real Apple Silicon, with
   an architecture-neutral scalar fallback and an explicit unimplemented x86
   status.
5. PL0 records a byte-identical keep/remove decision, and PL1 separately records
   geometry reuse, packed topology, and any host-topology representation result.
6. Persistent buffers, each upload-ring depth, and packed content caching each
   have a terminal decision after the staged Stage 1 integration boundary.
7. Strict Float32 correctness and all Metal validation gates pass on real Apple
   hardware.
8. The final report separates planner, scalar/NEON CPU stages, kernel GPU time,
   backend wall, cold pipeline creation, host packing/allocation/cache, and
   sustained behavior.
9. MK4 records `SKIPPED`, `KEPT`, or `REMOVED_AFTER_TEST`; it never blocks or
   weakens the strict Float32 path.
10. No source or contract owned by the Tauri/UI or staged planner worktree
   was changed without an explicit serialized integration checkpoint.
11. Pre-GPU and backend work are committed in separate worktrees from one
    reviewed baseline, then merged and revalidated only in the integration
    worktree.
