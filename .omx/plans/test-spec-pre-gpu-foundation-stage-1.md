# Test Specification: Staged Pre-GPU Foundation, Stage 0/1

## Status

- Current revision: staged revision 3, submitted with PRD revision 3
- Stage 0 is authorized for a later execution lane.
- Stage 1 is implementation-ready but executes only if the Stage 0 proceed gate
  passes.
- This planning run does not authorize source implementation.

## 1. Purpose

Prove only two things:

1. the existing benchmarks can expose planner and phase-separated total time
   without changing execution results;
2. if planner time is material, bounded parallel construction of independent
   unique AxisPlans is exact and improves the measured Metal benchmark enough
   to justify keeping it.

This specification does not gate or pre-authorize a scheduler, persistent
cache, packing ABI, cancellation system, CUDA backend, or Vulkan backend.

## 2. Existing Benchmark Anchors

The following successful local results are retained as sanity anchors:

```text
core: banded_us=80.992, batch32_ms=9.335, batch_candidates_per_s=3427.944
metal full: cpu_ms=1538.473, metal_ms=111.097, speedup=13.848x
metal correctness: maximum_metric_error=5.61e-8, valley_step_distance=0
metal memory: workspace=168.750 MiB, working_set=252.230 MiB
README historical five-run Metal median: 125.409 ms
```

They are not one comparison population:

- `banded_us` is a calibrated five-sample median for one `854 -> 600`
  bicubic AxisPlan and is useful as a single-plan microbenchmark.
- `batch32_ms` is one CPU execution timing over 32 candidates that reuse the
  same two plans; it is not a 32-unique-plan measurement.
- the Metal full benchmark builds its 1000 plans before `cpu_ms` and `metal_ms`
  begin, so those numbers cover execution and correctness, not planner or scan
  wall time;
- the README Metal median and the fresh `111.097 ms` one-shot came from
  different sampling protocols and must never be combined.

Therefore these values may detect a gross fixture or execution change, but they
must not be called the Stage 0 median, used as the denominator of a 5% gate, or
mixed with the Stage 1 decision population. The fresh Stage 0 serial run below
creates the only planner/total decision baseline.

## 3. Stage 0 Measurement Contract

Extend the existing core and Metal benchmark surfaces; do not introduce a
general benchmark framework.

### Identical planner boundary

The serial and later batch modes use the same measurement helper and boundary:

1. Construct the source fixture, active values, candidate ids, and complete
   `AxisPlanRequest` array before either planner timer starts.
2. Give every measured mode/sample fresh planner/cache state; no plan pointer or
   lookup state survives from a warmup or previous sample.
3. Start `plan_ms` before the first exact-key, deduplication, or cache lookup.
4. Stop `plan_ms` only after plan pointers have been scattered into original
   request order and attached to all candidates.
5. Start the unchanged CPU or Metal execution timer only after planning is
   complete. Stage 0/1 does not overlap planner and execution.

`cpu_ms` and `metal_ms` retain their current execution-only boundaries. Report
`cpu_total_ms = plan_ms + cpu_ms` and
`metal_total_ms = plan_ms + metal_ms`; these are phase-separated totals, not
full product or process wall time.

### Sampling and JSON

- Perform one unrecorded warmup with disposable state, followed by 21 recorded
  serial samples.
- Record every raw sample, median, and MAD for planner, execution, and total
  metrics. Also record each
  `plan_share_i = plan_ms_i / metal_total_ms_i` and its median.
- Record UTC timestamp, complete arguments, sample count, planner mode, host,
  compiler, build type/flags, executable identity, relevant source identity,
  and deterministic fixture/generator identity.
- Keep the current correctness, valley, workspace, and working-set fields and
  assertions.

Each invocation receives a final JSON path inside a newly created UTC run
directory. It must fail when that final path already exists, create a sibling
temporary file exclusively, flush and close it, and atomically rename it to the
final name only after the complete JSON is valid. A failed run leaves no final
JSON. Do not create a mutable `latest` link, retained-result database, or
promotion step.

### Stage 0 gate

Stage 0 passes measurement validity only when the build/tests and existing
`--assert` checks pass, JSON identities and sample counts are complete, and all
21 samples use the frozen boundary. Historical timings are diagnostic only and
do not impose a 5% comparison gate.

Record exactly one Stage 0 outcome:

- `PROCEED_STAGE1` when
  `median(plan_ms_i / metal_total_ms_i) >= 0.10`;
- `STOP_AND_REDIRECT_GPU` when the valid median is below `0.10`;
- `STAGE0_BLOCKED` when measurement or correctness is invalid; repair only the
  benchmark defect and rerun Stage 0.

Only `PROCEED_STAGE1` authorizes the later execution lane to implement Stage 1.

## 4. Stage 1 Functional Tests

Add `engine/tests/axis_planner_test.cpp`. The helper and its tests remain
private to `engine/src/planner`; no installed header or public error ABI is
introduced.

### Keying and in-call deduplication

- Empty input returns empty vectors and zero counts without launching workers.
- One request builds one plan through the serial fast path.
- 1000 identical requests build once and return 1000 equal pointers.
- Alternating A/B requests build twice and preserve request order.
- Requests that differ in any existing `PlanKey` field remain distinct using
  the current exact-bit semantics; this includes non-default bicubic B/C and
  floating-point bit distinctions.
- The helper uses no persistent/shared cache, and no state crosses calls.

### Exactness and ordering

Compare each scalar by exact value/bit representation and each vector by exact
element bytes against serial `build_axis_plan()`. Do not compare struct padding.
Cover:

- Bilinear;
- default Bicubic and custom Bicubic B/C;
- Spline16, Spline36, and Spline64;
- Lanczos3 and Lanczos8;
- representative existing source/destination sizes, active lengths, shifts,
  taps, and border cases.

Repeat with explicit worker counts 1, 2, 4, and 8 plus auto mode. Build
completion order must not change request order, plan bytes, or pointer sharing.

### Bounded concurrency and failure

- Empty input reports `effective_worker_count=0`. Non-empty batches of at most
  two unique keys and any one-worker request report/use one effective worker.
- Larger auto batches use no more than
  `min(unique_count, max(1, min(hardware_concurrency, 8)))` workers. Explicit
  counts clamp to `[1, unique_count]`.
- A focused test hook proves `peak_active_builds` never exceeds the reported
  effective count.
- Injected single and multiple failures stop claiming new keys after failure is
  observed, join all already-started workers, expose no partial batch result,
  and rethrow the failure belonging to the lowest stable first-occurrence key.
- Existing `AxisPlanCache` behavior and build count remain unchanged; Stage 1
  does not add cross-call single-flight or publication.
- Run the focused planner test under ThreadSanitizer.

Only benchmark pre-execution orchestration may consume the helper. Production
`metal_backend.mm`, `getnative.metal`, public Metal headers, and Metal engine
interfaces remain byte-for-byte unchanged; under that invariant, the focused
CPU planner TSan lane is the Stage 1 race gate.

## 5. Stage 1 Paired Performance Protocol

### Cases

| Requests | Unique keys | Purpose |
| ---: | ---: | --- |
| 1 | 1 | serial-fast-path overhead |
| 2 | 2 | serial-fast-path overhead |
| 32 | 32 | bounded scaling characterization |
| 1000 | 1000 | primary independent-plan and Metal-total gate |
| 1000 | 1 | in-call exact-key deduplication, not a cache case |

Characterize worker counts 1, 2, 4, 8, and auto in the core benchmark. The
adoption comparison uses auto mode for the existing 1000-unique Metal fixture.

### Pair construction

- Serial and batch modes run in the same final executable invocation with the
  same precomputed requests and fixture.
- Each mode receives fresh per-sample state.
- Pair 1 runs serial then batch, pair 2 runs batch then serial, and so on.
- Iteration/tuning runs may use 7 pairs. Only a fresh 21-pair run may decide
  adoption.
- Record the order and both raw values for every pair. Never discard selected
  samples because of their result.
- A source, executable, arguments, fixture, thermal policy, or host change
  invalidates the whole run and starts a new immutable run; it does not permit
  mixing samples.

For each metric define:

```text
delta_i = (batch_i - serial_i) / serial_i
paired_improvement = -median(delta_i)
paired_delta_MAD = median(abs(delta_i - median(delta_i)))
```

The decision population is valid only when paired-delta MAD is at most `0.025`
for both `plan_ms` and `metal_total_ms`. A first noisy result is
`NO_DECISION_NOISY` and reruns the unchanged final executable once in a new UTC
directory. A second noisy result cannot adopt or revise Stage 1.

### Required gates

- All exactness, ordering, concurrency, failure, existing unit/conformance, and
  `--assert` checks pass.
- For the 1000-unique case,
  `median(serial_plan_ms_i / batch_plan_ms_i) >= 2.0`.
- For each 1- and 2-request case, define
  `overhead_i = batch_plan_us_i - serial_plan_us_i`. Require
  `median(overhead_i) <= max(0.10 * median(serial_plan_us_i), 10 us)`.
- Metal execution-only paired median regression is at most 5%.
- The 1000-unique paired `metal_total_ms` improvement is at least 5%.
- Existing Metal tolerance, valley, workspace, and working-set assertions stay
  green.

No historical timing in Section 2 participates in these calculations.

For the one permitted `REVISE_ONCE`, a narrow miss means exactly one of these:

- planner speedup is at least `1.8x` but below `2.0x`;
- Metal-total paired improvement is at least 3% but below 5%;
- a 1/2-request overhead exceeds its adoption bound but satisfies
  `median(overhead_i) <= max(0.15 * median(serial_plan_us_i), 15 us)`.

Every other gate must already pass, and profiling must locate one cause inside
the current Stage 1 file set without changing the API, cache, or concurrency
contract. A result outside these bands is not a revision candidate.

## 6. Verification Commands

The following existing-anchor commands have already passed and remain useful as
smoke checks:

```sh
build/engine-perf/getnative_core_benchmark --assert
build/engine-metal/getnative_metal_benchmark --full --assert
```

### Stage 0 only

Run this stage before any batch planner implementation:

```sh
cmake -S engine -B build/stage0-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DGETNATIVE_ENABLE_METAL=ON \
  -DGETNATIVE_BUILD_UPSTREAM_CONFORMANCE=ON
cmake --build build/stage0-release --parallel
ctest --test-dir build/stage0-release --output-on-failure

mkdir -p artifacts/stage0
stage0_run="artifacts/stage0/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir "$stage0_run"
build/stage0-release/getnative_core_benchmark \
  --planner-mode serial --samples 21 \
  --json-out "$stage0_run/core-serial.json" --assert
build/stage0-release/getnative_metal_benchmark \
  --full --planner-mode serial --samples 21 \
  --json-out "$stage0_run/metal-serial.json" --assert
```

Inspect the recorded `plan_share` median and write the Stage 0 outcome. Stop
here unless it is exactly `PROCEED_STAGE1`.

### Conditional Stage 1 release gate

Run only after Stage 0 records `PROCEED_STAGE1` and Stage 1 is implemented:

```sh
cmake -S engine -B build/stage1-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DGETNATIVE_ENABLE_METAL=ON \
  -DGETNATIVE_BUILD_UPSTREAM_CONFORMANCE=ON
cmake --build build/stage1-release --parallel
ctest --test-dir build/stage1-release --output-on-failure

mkdir -p artifacts/stage1
stage1_run="artifacts/stage1/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir "$stage1_run"
build/stage1-release/getnative_core_benchmark \
  --compare-planner-modes --samples 21 \
  --json-out "$stage1_run/core-paired.json" --assert
build/stage1-release/getnative_metal_benchmark \
  --full --compare-planner-modes --samples 21 \
  --json-out "$stage1_run/metal-paired.json" --assert
```

The compare mode owns alternating order within one process; two independent
serial/batch command invocations do not satisfy the paired protocol.

### Focused ThreadSanitizer gate

```sh
cmake -S engine -B build/stage1-tsan \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DGETNATIVE_ENABLE_METAL=OFF \
  -DGETNATIVE_BUILD_UPSTREAM_CONFORMANCE=OFF \
  -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread'
cmake --build build/stage1-tsan --parallel
TSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir build/stage1-tsan --output-on-failure \
  -R '^getnative_axis_planner_tests$'
```

If the local compiler/runtime cannot execute TSan, Stage 1 is not fully verified
until a supported host runs the same focused test.

## 7. Decision Output and Stop Rule

End Stage 1 with one short table sourced only from the final paired artifact:

| Metric | Serial | Batch | Paired delta/speedup | Gate |
| --- | ---: | ---: | ---: | --- |
| 1000-unique plan median | | | | |
| Plan paired-delta MAD | | | | |
| 1-request plan median | | | | |
| 2-request plan median | | | | |
| Metal execution median | | | | |
| Metal total median | | | | |
| Metal-total paired-delta MAD | | | | |
| Correctness/conformance/TSan | | | | |

Then record exactly one recommendation:

- `ADOPT_AND_STOP`: every correctness, variance, regression, `>=2x` planner,
  and `>=5%` Metal-total gate passes. Keep Stage 1 and stop; write another small
  plan only when this evidence selects a later direction.
- `REVISE_ONCE`: correctness, TSan, variance, and execution regression are
  green; exactly one performance gate falls inside the Section 5 near-miss
  bands; and profiling names one bounded local bottleneck. Permit one
  implementation revision and one fresh unchanged-gate rerun only.
- `REVERT_AND_REDIRECT`: any valid non-adopt result that does not satisfy every
  `REVISE_ONCE` predicate, noise that remains above `0.025` after the one
  unchanged rerun, or the one revision still missing any adoption gate. Remove
  the Stage 1 batch path, retain Stage 0 instrumentation and immutable evidence,
  and select a different measured direction.

No recommendation starts Stage 2 implementation from this document.
