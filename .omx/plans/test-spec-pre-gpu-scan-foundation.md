# Test Specification: Pre-GPU Scan Engine Foundation

> Superseded by `test-spec-pre-gpu-foundation-stage-1.md`. This file is retained
> as exploration history and is not the current execution contract.

## 1. Purpose

Prove that the planner/cache/scheduler/GPU-packing optimization program improves
full scan performance without changing AxisPlan bytes, strict CPU behavior,
candidate ordering, Metal correctness, bounded-memory behavior, cancellation,
or optional-backend startup semantics.

This specification is a release gate, not a suggestion list. Performance claims
must use fresh output produced after the relevant implementation milestone.

## 2. Evidence classes

1. Exactness: bitwise plan and CPU result comparisons.
2. Concurrency: physical-build counters, worker/scratch ceilings, race tools,
   and deterministic stress tests.
3. Layout: compile-time sizes/offsets and byte-exact packed fixtures.
4. Integration: bounded prepared slices through CPU and common packing through
   Metal.
5. End to end: cold/warm full-pipeline wall time, memory, cancellation, and
   optional-runtime startup.
6. Observability: counter conservation and stage-timing schema validation.

## 3. Fixed fixture matrix

### 3.1 AxisPlan exactness matrix

Cross the following dimensions with pairwise coverage plus all named edge cases:

- Source/destination: `37->23`, `64->43`, `854->600`, `1080->720`,
  `1920->1440`, and destination close to filter support.
- Active length: integer, `.125`, `.25`, `.375`, and a representative value
  below destination size.
- Shift: `0`, `-0` as a separate key case, `+0.125`, `-0.375`, and half-pixel.
- Border: zero, repeat, mirror.
- Filter: bilinear; bicubic `(0,0.5)` and `(0.25,0.4)`; Lanczos taps 1 through 8
  plus core-only taps 15; Spline16/36/64.

For each fixture compare every scalar field and every vector element. Floating
values compare by bit pattern, not tolerance. Record a stable test-only digest
for failure diagnostics, but do not make a machine-dependent `std::hash` the
oracle.

### 3.2 Scan workload matrix

| Workload | Consumers | Unique keys | Axes/shapes | Purpose |
| --- | ---: | ---: | --- | --- |
| S1 | 1 | 1 | V/B7 | small-batch overhead |
| S2 | 2 | 2 | H+V/B7 | two-axis coarse parallelism |
| S3 | 32 | 32 | V/B7 | CPU batch and planner crossover |
| S4 | 1000 | 1000 | V/B7 fractional heights | primary cold performance gate |
| S5 | 1000 | 500 | V mixed repeats | partial cache/dedup behavior |
| S6 | 1000 | 1 | V/B7 shared plan | single-flight and pack dedup |
| S7 | 256 | mixed | H/V/both, B3/B7/generic | signature/order/packing |
| S8 | repeated frames | same as S7 | warm cache | cross-job/session reuse |

Source fixtures include contiguous and padded-stride GRAY F32 images. The
primary performance fixture remains `1920x1080`, bicubic `(0,0.5)`, 1000
fractional vertical candidates, crop 5, threshold `0.015`, p=1.

## 4. M0 baseline and telemetry tests

### 4.1 Benchmark output schema

The JSON report has a version and includes:

- fixture id, candidate count, unique key count, axes/shape distribution;
- build type, compiler/version, strict flags, host model/CPU count/RAM;
- cache state (cold/warm), configured workers and every byte limit;
- per-stage wall nanoseconds;
- request/hit/wait/build/pending-claim/admission/tile/window/byte counters;
- CPU or GPU result checksum, maximum metric error, valley distance;
- process peak RSS, active/returned distinct-plan bytes, and explicit
  planner/host/GPU byte peaks;
- sample count, median, p95, median absolute deviation, and individual samples;
- raw paired emitted/unique pack and upload microbench durations, byte counts,
  allocation/staging mode, paired deltas, direct in-pipeline stage upper bound,
  and the explicitly labelled `addressable_stage_budget`;
- SHA-256 build/source/binary/metallib manifest identifier.

Schema tests reject missing fields, negative durations, unknown version, or
non-finite metrics.

### 4.2 Counter conservation

For every completed run:

- `requests >= unique_keys`.
- `unique_keys = ready_dispositions + existing_flight_dispositions
  + new_build_claim_dispositions` for the batch's first acquisition of each
  key; cumulative cache counters are not substituted for these per-batch
  dispositions.
- `physically_built_keys <= new_build_claim_dispositions`; every unbuilt claim
  terminates as an explicit pre-submit abandonment/failure.
- `builds_completed + build_failures = builds_started` after all workers join.
- peak pending claims, active retained-plan bytes, scratch bytes, and output bytes
  are each within their configured service-wide ceiling.
- `result_count = candidate_count`.
- every original result index is written exactly once.
- emitted coefficient bytes are `<=` logical per-consumer coefficient bytes.
- total wall duration is at least each nested stage and documents overlap when
  the sum of stages exceeds total wall.

Cancellation/failure reports identify incomplete stages and do not claim the
completed-run conservation equation.

### 4.3 Baseline method

- Release build with warnings enabled and strict FP flags.
- One unmeasured warmup per executable/device.
- At least 21 independent measured samples for any p95/performance claim;
  report median, p95, and median absolute deviation. A shorter seven-sample run
  is smoke evidence only.
- Each cold sample uses a fresh cache/service session; warm samples explicitly
  reuse one named session. Use child processes when process peak RSS is part of
  the claim.
- Do not run CPU and Metal performance cases concurrently.
- Record thermal/power caveats and reject samples with unrelated load or a
  changed build hash.
- Preserve the old execution-only timer alongside full-pipeline timing.
- Hash relevant sources, CMake inputs, benchmark binaries, and embedded
  metallib. Do not rely on git `HEAD` in this untracked checkout.

### 4.4 Addressable-stage calibration

For S4-S7, run at least 21 paired samples for each applicable pack and upload
primitive:

- The emitted case uses the exact current per-consumer coefficient payload; the
  unique case uses a benchmark-only payload containing each distinct plan once.
- Both sides use the same copy implementation, allocation/staging mode, device,
  payload ordering, and build manifest. Alternate pair order across samples and
  record all raw durations and bytes.
- Compute each component as the median of paired
  `emitted_duration - unique_duration`, not the difference of two independent
  medians. A non-positive component contributes zero to the target and remains
  visible in the raw report.
- Compute `addressable_stage_budget` exactly as the PRD specifies. Mark it as a
  calibration budget; never call it measured removable/avoidable wall time.
- Never infer duration by multiplying total time by a redundant-byte fraction.
  If the production primitive cannot be isolated faithfully, contribute zero
  for that component and record the limitation.
- The directly timed production `plan + host_pack + upload` stage total remains
  a separate upper-bound field and must not be added again to the calibration
  budget.

### 4.5 Shared local-valley oracle

Add `engine/tests/scan_metrics_test.cpp` and exercise the exact shared helper:

- empty input returns no valley; one finite value returns index 0;
- `[1,2]` selects endpoint 0 and `[2,1]` selects endpoint 1;
- `[2,1,1,2]` is one plateau valley represented by index 1;
- an all-equal non-empty vector is one valley represented by index 0;
- `[1,2,2,1]` selects the two endpoint valleys and not the middle plateau;
- equal-valued valleys rank by ascending representative index;
- `-0` and `+0` occupy one numeric-equality plateau;
- NaN and either infinity are rejected;
- top-k selection is by metric then index, while matching re-sorts selected
  indexes ascending and pairs them positionally;
- exact and one-step-shifted sets pass; a two-step shift, unequal selected
  cardinality, a many-to-one candidate, or a case that would require rematching
  fails deterministically.

### 4.6 Final full-wall arithmetic

Apply the final gate independently to CPU/S4 and Metal/S4-S7 with at least 21
M0 and final samples. For every backend/workload pair, the benchmark computes:

```text
B = M0 addressable_stage_budget
T0 = M0 total-wall median
Tf = final total-wall median
N = max(0.01 * T0, 3 * M0 total-wall MAD, 3 * final total-wall MAD)
target = min(0.20 * T0, 0.70 * B)
observed_saving = T0 - Tf
```

- `PASS` requires `B > N` and `observed_saving >= max(target, N)`.
- `B <= N` yields `NO_MEASURABLE_ADDRESSABLE_BUDGET`, never `PASS`.
- `B > N` with insufficient saving yields `MEASURED_TARGET_MISS`.
- Reports retain all raw samples and never aggregate workloads or backends.
- Either non-pass result requires the PRD's M7 native performance-verifier
  disposition and cannot be presented as a successful optimization claim.

## 5. M1 key/cache unit tests

Add `engine/tests/axis_plan_cache_test.cpp`.

### 5.1 Key normalization

- Every coefficient-affecting field produces a distinct key.
- Raw irrelevant B/C/taps fields for bilinear and spline produce the same key.
- Raw irrelevant B/C for Lanczos produce the same key; taps remain distinct.
- Bicubic B/C remain exact-bit distinct; raw taps canonicalize to support 2.
- Relevant `+0` and `-0` remain distinct in P0 unless an exact plan comparison
  later approves canonicalization.
- Invalid requests fail before entering the cache or incrementing build stats.
- Key equality and hash equality satisfy the unordered-container contract.

### 5.2 Single-flight success

Use a start barrier for 32 threads calling one cold key:

- exactly one acquisition returns `new_build_claim`; all others return
  `existing_flight` or observe ready after publication;
- all returned pointers are equal;
- cache resident entry count is one;
- `builds_started == 1`, `builds_completed == 1`;
- at least one waiter is observed on a deliberately large plan;
- no thread holds the cache lock while the build executes or waits;
- existing-flight waiters consume zero planner worker slots.

Repeat with two interleaved keys and prove `builds_started == 2` and peak active
builds can exceed one in the planner service.

### 5.3 Failure and retry

- A deterministic invalid/overflowing request reaches all current waiters with
  the same exception category/message contract.
- Failed state is removed, resident bytes stay unchanged, and a later valid
  request for another key succeeds.
- For an injectable test-only failing builder or failpoint, retrying the same
  key starts one new build, not one per former waiter.
- A claim abandoned before submission wakes all waiters with the typed terminal
  result, removes only its identity-matched flight, and permits a later retry.
- A cancelled waiter exits without cancelling or corrupting the shared flight.

### 5.4 Clear/eviction/lifetime

- `clear()` removes ready entries and advances generation.
- An old in-flight build may satisfy old waiters but cannot repopulate the new
  ready generation. A new same-key caller after clear creates a new-generation
  flight; old completion can neither admit nor erase it.
- LRU order updates on ready hit; entry and byte ceilings each evict correctly.
- An oversize plan returns successfully but is not admitted.
- Externally held plans remain valid and byte-identical after eviction/clear.
- Repeated churn never reports resident bytes above the configured ceiling.
- Cache/service state destruction is invoked only after every cache API call,
  claim, and waiter handle has reached terminal state and joined. Terminal
  cleanup removes all flight indexes and destroys ready ownership last; this
  test never uses concurrent object destruction as cancellation.

### 5.5 Race validation

Run the cache stress target under ThreadSanitizer with equal-key misses, distinct
misses, stats reads, clear, and destruction after joins. Any data race, lock
order failure, or use-after-free fails M1.

## 6. M2 AxisPlanService tests

Add `engine/tests/axis_planner_test.cpp`.

### 6.1 Dedup and stable output

- Empty input returns empty output and zero tasks.
- 1000 copies of one request return 1000 pointers to one plan and one physical
  build.
- Alternating A/B requests return input-order pointers and two physical builds.
- First-occurrence key order is deterministic across worker counts 1, 2, and
  auto.
- A fully warm batch starts zero builds.
- Two concurrent callers on one cold service admit exactly one batch at a time.
  The waiting call acquires zero cache claims until the first releases admission;
  it then observes ready plans, and total physical builds remain one per key.
- Admission order does not alter either call's request-order result. A waiting
  caller cancelled before admission returns typed cancellation with zero claims
  and leaves the active call unchanged.
- Concurrent direct cache acquisitions still prove the underlying single-flight
  protocol independently of the serialized P0 service-call policy.

### 6.2 Byte exactness

For the full AxisPlan fixture matrix, compare `build_many()` output with direct
serial `build_axis_plan()` output field by field and bit by bit. Repeat for
worker counts 1 and maximum. No tolerance is allowed.

### 6.3 Limits and fallback

- Worker count never exceeds configured or hardware ceiling.
- Small work below the calibrated threshold uses the serial path.
- Capped/counting scratch allocation blocks additional unique work rather than
  exceeding the logical byte ceiling.
- A single estimated plan above the ceiling fails before its first large
  allocation.
- Checked estimation rejects integer overflow.
- Actual scratch and output logical high-water counters never exceed the claim;
  inject an intentionally under-sized test claim and require a bounded failure
  rather than silent over-allocation.
- Queue capacity applies backpressure without spinning.
- Request count above `maximum_plan_requests_per_batch`, unique keys above
  `maximum_pending_plan_claims`, or checked distinct output upper bound above
  `maximum_retained_plan_bytes_per_batch` fails before the first cache claim.
- A request whose count/byte sum overflows fails before acquisition.
- During a maximum legal batch, `peak_pending_claims`, active distinct returned
  plan bytes, scratch, and output bytes remain within service ceilings even when
  the worker queue is full; queue capacity is not accepted as proof of those
  bounds.
- Returned `retained_plan_bytes` counts pointer-distinct plans once. Repeated
  request pointers do not multiply it, and `PreparedScanSlice` charges the same
  value to its host-plan-byte ceiling until slice release.

### 6.4 Cancellation/failure/destruction

- Stop requested before call submits no build.
- Stop during a batch submits no later key after observation, joins workers,
  and returns typed cancellation.
- Already-started successful plans may be cache hits afterward.
- One key failure stops new submission, joins started work, and rethrows only
  after the service reaches a stable state.
- `request_stop()` while one call is active and another waits for admission
  wakes both, rejects new calls, creates no post-observation claim, abandons
  every unsubmitted claim, and lets already-running builders publish terminally.
- `shutdown()` is idempotent, may execute while those calls unwind, joins fixed
  workers exactly once, and does not destroy per-call/shared state beneath them.
- The owner joins all threads executing public members after `shutdown()` and
  only then invokes the destructor. Idle, used, explicitly shut down, and
  destructor-initiated shutdown paths each destroy terminal cache state once.
- A test that attempts a new `build_many()` or `clear_cache()` after stop begins
  receives the documented stopped-service error; `cache_stats()` remains safe
  until public-call threads are joined.

### 6.5 Performance

- S4 cold unique-key median throughput is at least 4x M0 serial on the M4 Max.
- S1/S2 latency is within `max(5%, 5 us)` of direct serial build.
- Warm S6 has zero builds and no more than 5% regression from the M1 cache-only
  baseline.
- Peak RSS is below 512 MiB.

### 6.6 Bounded remediation if a mandatory gate fails

- Do not enter remediation when all initial M2 gates pass; record
  `SKIPPED_TRIGGER_FALSE` for both remedies.
- Otherwise test per-worker scratch reuse once, then exact capacity reservation
  once. Each uses the unchanged 21-sample M2 command and a temporary switch.
- Retain an experiment only when it improves the failing median by at least 5%
  or the failing peak byte/RSS value by at least 10%, preserves exact plan bytes
  and sanitizer results, and makes no other M2 gate fail.
- Remove a rejected experiment and rerun the reference before the next. If the
  mandatory M2 gates remain red after both attempts, assert `BLOCKED_M2`; no M3
  artifact is valid.

## 7. M3 bounded scan-slice tests

Add `engine/tests/scan_batch_test.cpp`.

### 7.1 Validation and plan table

- H requires horizontal only, V requires vertical only, both requires both;
  missing or extra incompatible requests are rejected with the candidate index.
- Identical normalized H and V requests share one unique plan-table entry.
- Unique plans use first-occurrence order within each slice independent of
  build completion; every candidate retains its global input index.
- Repeated candidate ids do not affect internal indexes or result scatter.
- Maximum candidate/plan index overflow is rejected before narrowing to u32.
- Candidate-count and host-plan-byte limits split adjacent input into bounded
  slices without dropping or reordering a consumer.

### 7.2 CPU compatibility

For S1-S7, prepare, execute, and scatter one slice before preparing the next:

- materialized current `CandidateAnalysis` matches the prepared references;
- prepared CPU results equal scalar results bit-for-bit;
- prepared CPU results equal current parallel `analyze_batch_f32` bit-for-bit;
- worker counts 1, 2, and auto produce identical ordered vectors;
- padded source stride produces the same result as an equivalent contiguous
  image.
- The benchmark retains `m2_direct` and alternates 21 pairs with `m3_slice` for
  cold and warm S1-S7 sessions. For CPU execution and total wall separately,
  compute `delta_i = m3_slice_i - m2_direct_i` and
  `ratio_i = delta_i / m2_direct_i`.
- Evidence is invalid and must be rerun unchanged when any
  `m2_direct_i <= 0` or `MAD(ratio_i) > 0.02`; noise never widens the threshold.
  S1-S2 require
  `median(delta_i) <= max(0.05 * median(m2_direct_i), 5 us)`; S3-S7 require
  `median(ratio_i) <= 0.05`.
- M0-to-M2 planner gain is reported separately and is never subtracted from M3
  time. On failure, evaluate at most two variants limited to slice capacity and
  allocation reservation. If both fail, M3 remains blocked and M4 does not
  begin.

### 7.3 Scatter

- Simulate out-of-order completion and scatter by `input_index`.
- Reject duplicate, missing, or out-of-range result indexes.
- Failure/cancellation returns no completed result vector.

### 7.4 Shared resource budget

- CPU planner and candidate execution never overlap in the P0 phase-separated
  path, even across slice boundaries.
- Reported planner workers and CPU workers are each within the same session
  ceiling.
- Auto policy is tested as a versioned, configurable pure policy across reported
  hardware counts 1, 2, 7, 8, and 16; assertions enforce bounds/headroom, not a
  permanently frozen reserve count.

## 8. M4a-M4c GPU batch CPU-only tests

Add `engine/tests/gpu_batch_test.cpp`; it must link only `getnative_core` and
the pure C++ GPU batch library.

### 8.1 ABI/layout

- `sizeof(GpuAxisPlanDescriptor) == 64`, alignment 16.
- `sizeof(GpuAnalysisJob) == 40`.
- Assert `offsetof` for all fields against the documented ABI.
- Validate host scalar widths and reject unsupported endianness if portability
  code does not explicitly handle it.
- Hand-author B3, B7, generic, H, V, and both packed inputs from exact integer
  and IEEE-754 constants. Their serialized POD/array SHA-256 values must match
  across supported macOS and Windows compilers; planner-generated bytes are not
  the cross-compiler fixture oracle.
- The M4a/M4b base fixtures are always present. If M4c is kept, add hand-authored
  repeated-plan S5/S6 fixtures whose canonical first-occurrence table,
  descriptors, bases, and arrays join the same final golden. If M4c is removed,
  those optional fixture entries are absent and the base golden remains exact.
- `getnative_gpu_batch_tests --write-fixture-hashes <path>` writes those
  deterministic hashes without changing the checked-in golden and exits
  nonzero on any internal mismatch.

### 8.2 Shape and workspace

- B3 is exactly half-bandwidth 1/forward width 2.
- B7 is exactly 3/4.
- Every other supported pair through 15/16 is generic.
- Invalid/null plans, source mismatch, width zero, and values above limits fail.
- H, V, both, and both forward orders match current Metal workspace values.
- Candidate, tile, and window sums use checked multiplication/addition.
- One candidate over the workspace limit fails; otherwise tile size adapts.

### 8.3 Tile ordering

- Adjacent equal signatures join until candidate, workspace, plan-byte, or tile
  limits stop the tile.
- A signature change closes the tile; later matching signatures do not move in
  P0.
- Every input candidate appears once and in order.
- Window boundaries preserve global result indexes.

### 8.4 Plan dedup and descriptors

- M4a shadow mode emits the current per-candidate copies and compares the entire
  common packed representation byte-for-byte with the old Metal helper.
- Mandatory M4b preserves the entire M4a representation byte-for-byte while
  changing only backend-local queue/cancellation behavior.
- Optional M4c S6 emits one coefficient copy per submission window and 1000
  candidate descriptors/results.
- Two candidates sharing one plan have equal coefficient bases and distinct
  workspace bases.
- A both-axis candidate sharing the same plan for H and V emits one coefficient
  copy and two correctly directed descriptors.
- Unique plan arrays are flattened in plan-table first-occurrence order.
- Forward rows must be contiguous and exact width; invalid rows fail.
- Negative/out-of-range transpose or forward indexes fail.
- Every host offset is checked before u32 narrowing.
- For M4c repeated-plan fixtures, do not require old/new packed bytes to match:
  decode descriptors and prove their shared bases reference the exact expected
  coefficient values, then prove ordered result equivalence. Non-shared
  fixtures remain byte-identical to M4b.

### 8.5 Partial merge

- Groups merge candidate-major then ascending group index with Float32-to-double
  conversion exactly matching `engine/src/backend/metal/metal_backend.mm:972-978`.
- Result scatter uses original input indexes.
- Non-divisible pixels, zero groups, wrong partial length, duplicate indexes,
  and non-positive pixel count fail.

### 8.6 CPU-only optional-backend build

Configure/build/test with Metal, CUDA, and Vulkan disabled. No GPU SDK header or
library may be needed to compile `gpu_batch_test` or start `getnative-engine`.

## 9. M4a-M4c Metal integration tests

Extend `engine/tests/metal_conformance_test.cpp`.

- Preserve all existing vertical/horizontal/all-shape/two-axis/forward-order/
  mixed-order/workspace/cancellation cases.
- In M4a, run the old and common packers in a bounded test/diagnostic shadow
  mode and compare bytes, descriptors, tiles, workspace values, partial merge,
  and ordered results.
- In mandatory M4b, preserve all M4a packed bytes and add only the calibrated
  queued-ahead/cancellation path. Its green report and manifest must become the
  retained Metal predecessor before any removable overlay starts.
- In optional M4c, add S6 shared-plan dedup and verify telemetry reports one
  coefficient copy per submission window; shared fixtures use semantic parity,
  not an impossible byte-identity requirement.
- Add plan-byte-bound window splitting and result scatter.
- Compare common pack layout against frozen pre-migration descriptors for B3,
  B7, generic, H, V, and both.
- Run padded-source, invalid-index, maximum u32 boundary, and nonfinite metric
  validation through the common path.
- Keep maximum metric error within the existing per-candidate bound and valley
  distance <=1.
- Run CPU and Metal metric vectors through the same section 4.5 helper. Each
  side independently selects valleys by plateau value then leftmost index and
  takes `min(5, valley_count)`. Require equal selected cardinality, sort selected
  indexes ascending, pair positionally, and require each distance `<=1`; no
  alternate or many-to-one matching is allowed.
- Starting in M4b, request stop from another thread after at least one tile has
  been submitted;
  prove no later window is submitted after observation and completion occurs
  within two measured tile durations under the calibrated queued-ahead policy.
- Keep workspace and total explicit working set each below 2 GiB.
- Compare M0 execution-only median; regression must be <=5%.

Remove the old/rollback packer only after mandatory M4b passes byte parity,
cancellation, correctness, memory, and <=5% execution-regression gates. M4b is
never removed by a no-benefit performance disposition. M4c is one removable
dedup overlay. Run 21 alternating same-process M4b/M4c pairs for S4-S7 and
compute `ratio_i = (m4c_i - m4b_i) / m4b_i`; non-positive M4b time or
`MAD(ratio_i) > 0.02` invalidates the unchanged run. `KEPT` requires emitted-
coefficient-byte reduction on S5/S6, `median(ratio_i) <= 0.05` for each S4-S7,
and either `median(ratio_i) <= -0.05` on S5 or S6, or >=20% reduction in S6's
combined emitted-host-coefficient plus uploaded-coefficient bytes and >=10%
total explicit-working-set reduction. If it misses, delete only M4c, run the
exact M4b reference-rerun command in section 12.3, and promote that fresh M4b
report/manifest as the stable predecessor. Run Metal tests with the exact API
and Shader Validation invocation in section 12.2. A CPU-only or compile-only
result is not Metal runtime evidence.

## 10. M5 pipeline tests if triggered

Add `engine/tests/scan_pipeline_test.cpp` using a deterministic fake GPU window
executor plus real Metal integration.

### 10.1 State machine/backpressure

- At most two bounded prepared slices/submission windows are resident/in flight.
- Producer blocks on the byte/candidate budget without spinning.
- Window N+1 may prepare while N executes, but result visibility remains ordered.
- Empty, one-slice, exact-boundary, and final-partial-slice streams complete.

### 10.2 Failure/cancellation matrix

Inject cancellation/failure during keying, cache wait, build, pack, before
submit, during submitted window, readback, and scatter:

- no new window is submitted after observation;
- producer and consumer both wake and join;
- an already-submitted Metal window reaches a safe boundary;
- no partial final result vector is returned;
- successful prior plans may remain cached;
- reusable buffers/session state are valid for the next run.

### 10.3 Parity and keep/remove gate

- Phase-separated and overlapped paths return identical ordered results and
  equivalent counters apart from overlap timing.
- Cancellation latency is <= two measured tile durations.
- Retain overlap only if full cold median improves >=10% over the stable
  retained predecessor without memory regression; otherwise delete the
  implementation, leave that predecessor unchanged, and keep compatible APIs.

## 11. M6 optional optimization tests

### 11.1 Common experiment protocol

- Run items sequentially and permit at most two implementation variants per
  item. Every reference and variant run has at least 21 samples.
- Every item reads the stable retained-predecessor artifact produced by the
  preceding decision. `KEPT` advances it only after all required gates pass;
  skip/removal leaves it byte-identical after the unchanged reference rerun.
- Trigger counters and the final disposition are present in JSON. Exactly one of
  `SKIPPED_TRIGGER_FALSE`, `KEPT`, or `REMOVED_AFTER_TEST` is required.
- A false trigger creates no source implementation. A rejected variant is
  deleted with its switch, then the unchanged reference command passes before
  the next item.
- Every variant reruns relevant exactness, Metal, cancellation, sanitizer, RSS,
  explicit-working-set, and ordered-result gates. A microstage-only win cannot
  override the full-wall keep threshold.

### 11.2 Shared horizontal intermediate reuse

- Trigger fixture proves identical normalized horizontal keys/geometry for at
  least 25% of S7 or S8 and horizontal inverse + forward time at least 10% of
  backend wall. Below either boundary produces `SKIPPED_TRIGGER_FALSE`.
- Mutation/alias tests prove concurrently executing candidates never share
  mutable workspace.
- `KEPT` requires at least 5% full-wall improvement, no more than 5% explicit
  working-set increase, and all semantic gates green; otherwise delete it.

### 11.3 Stable global signature bucketing

- Trigger fixture has at least 25% more adjacent tiles than the computed stable
  global lower bound and tile-layout + submit CPU time at least 10% of Metal
  wall. Exercise exact-boundary values on both sides.
- Whole-slice and at most one bounded variant scatter every result once to its
  original index and preserve cancellation latency.
- `KEPT` requires at least 5% S7 full-wall improvement, no more than 5% RSS/GPU
  working-set increase, absolute memory gates, and full conformance; otherwise
  delete it.

### 11.4 Cross-frame GPU-resident plan arena

- Trigger fixture uses at least three frames, at least 80% repeated plan-key hit
  rate, and coefficient pack + upload at least 10% of warm S8 wall. Exercise
  values immediately below every threshold.
- Version, device identity, clear, LRU eviction, capacity, device-loss
  simulation, cancellation, and stale-base rejection are covered.
- `KEPT` requires at least 10% warm S8 improvement, at most 5% cold S7
  regression, bounded cache/total GPU bytes, and semantic parity; otherwise
  delete it.

## 12. Verification command matrix

### 12.1 Required command surface

All commands run from the repository root and fail on any nonzero child exit.
M0 adds these CMake settings:

- `GETNATIVE_WARNINGS_AS_ERRORS=ON|OFF` (verification always uses `ON`);
- `GETNATIVE_SANITIZER=none|address-undefined|thread` (invalid or unsupported
  values fail configure; thread and address-undefined cannot combine).

New CTest names are stable and exact:

```text
getnative_scan_metrics_tests
getnative_axis_plan_cache_tests
getnative_axis_planner_tests
getnative_scan_batch_tests
getnative_gpu_batch_tests
getnative_scan_pipeline_tests
getnative_metal_tests
```

`getnative_scan_pipeline_benchmark` accepts the PRD's exact flag set. With
`--assert`, it exits nonzero unless the selected gate has `PASS` or the exact
explicit non-pass disposition permitted by that gate. Every JSON contains
`schema_version`, `gate`, `status`, workload/backend ids, raw samples, median,
p95, MAD, counters, byte peaks, command arguments, and manifest id.

The two mutable comparison files are:

```text
artifacts/pre-gpu-foundation/retained/macos-arm64/cpu.json
artifacts/pre-gpu-foundation/retained/macos-arm64/metal.json
```

Each is a byte-for-byte copy of one immutable milestone report; its sibling
`.sha256` is the matching immutable manifest. Commands never infer a predecessor
from a milestone number: `--previous` always names one of these stable files.
The controller records each promotion or no-op in
`artifacts/pre-gpu-foundation/retained/macos-arm64/ledger.jsonl` with backend,
old/new report SHA-256, gate, experiment, disposition, and source path.

Promotion is a deterministic gate action, not an executor choice:

- Initial mandatory-path evidence establishes the file after its required
  correctness/memory/concurrency commands are green. M2's initial result also
  establishes the current M2 implementation when it reports
  `REMEDIATION_REQUIRED`, so the first remedy has an exact reference.
- A mandatory successor with `PASS`, or an optional experiment with `KEPT`,
  copies its JSON and manifest over the relevant stable pair only after every
  command mapped in section 12.4 is green.
- `SKIPPED_TRIGGER_FALSE` makes no source change and no promotion.
  `REMOVED_AFTER_TEST` requires source/switch deletion and a fresh unchanged
  predecessor rerun. M5/M6 leave the already-proven stable pair unchanged and
  archive the rerun separately. M4c instead promotes its fresh mandatory M4b
  rerun; the M4b source/binary manifest identity must match the pre-M4c
  predecessor even though new timing samples change the report SHA-256.
- Mandatory M4b promotes only on `PASS` and cannot be removed by
  `NO_MEASURABLE_ADDRESSABLE_BUDGET` or `MEASURED_TARGET_MISS`. Those final
  statuses leave the already-truthful retained path unchanged and require M7 to
  report no successful optimization claim. M4c/M5/M6 use their own exact
  `KEPT`/skip/removal gates before M7.

### 12.2 macOS build, test, and sanitizer commands

Debug CPU warnings-as-errors:

```sh
cmake -E make_directory artifacts/pre-gpu-foundation/verification/macos-arm64
cmake -S engine -B build/pre-gpu-debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGETNATIVE_ENABLE_METAL=OFF \
  -DGETNATIVE_BUILD_UPSTREAM_CONFORMANCE=OFF \
  -DGETNATIVE_WARNINGS_AS_ERRORS=ON \
  -DGETNATIVE_SANITIZER=none
cmake --build build/pre-gpu-debug --parallel
ctest --test-dir build/pre-gpu-debug --output-on-failure \
  --output-log artifacts/pre-gpu-foundation/verification/macos-arm64/debug-cpu-ctest.log
```

Release CPU and upstream conformance:

```sh
cmake -S engine -B build/pre-gpu-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DGETNATIVE_ENABLE_METAL=OFF \
  -DGETNATIVE_BUILD_UPSTREAM_CONFORMANCE=ON \
  -DGETNATIVE_WARNINGS_AS_ERRORS=ON \
  -DGETNATIVE_SANITIZER=none
cmake --build build/pre-gpu-release --parallel
ctest --test-dir build/pre-gpu-release --output-on-failure \
  --output-log artifacts/pre-gpu-foundation/verification/macos-arm64/release-cpu-ctest.log
```

ASan+UBSan CPU/shared-contract targets:

```sh
cmake -S engine -B build/pre-gpu-asan \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DGETNATIVE_ENABLE_METAL=OFF \
  -DGETNATIVE_WARNINGS_AS_ERRORS=ON \
  -DGETNATIVE_SANITIZER=address-undefined
cmake --build build/pre-gpu-asan --parallel
ASAN_OPTIONS=halt_on_error=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build/pre-gpu-asan --output-on-failure \
  -R '^getnative_(core|geometry|scan_metrics|axis_plan_cache|axis_planner|scan_batch|gpu_batch|scan_pipeline)_tests$' \
  --output-log artifacts/pre-gpu-foundation/verification/macos-arm64/asan-ubsan-ctest.log
```

TSan concurrency targets:

```sh
cmake -S engine -B build/pre-gpu-tsan \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DGETNATIVE_ENABLE_METAL=OFF \
  -DGETNATIVE_WARNINGS_AS_ERRORS=ON \
  -DGETNATIVE_SANITIZER=thread
cmake --build build/pre-gpu-tsan --parallel
TSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir build/pre-gpu-tsan --output-on-failure \
  -R '^getnative_(axis_plan_cache|axis_planner|scan_batch|scan_pipeline)_tests$' \
  --output-log artifacts/pre-gpu-foundation/verification/macos-arm64/tsan-ctest.log
```

Release Metal with API and Shader Validation:

```sh
cmake -S engine -B build/pre-gpu-metal \
  -DCMAKE_BUILD_TYPE=Release \
  -DGETNATIVE_ENABLE_METAL=ON \
  -DGETNATIVE_WARNINGS_AS_ERRORS=ON \
  -DGETNATIVE_SANITIZER=none
cmake --build build/pre-gpu-metal --parallel
MTL_DEBUG_LAYER=1 \
MTL_SHADER_VALIDATION=1 \
MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING=1 \
MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
MTL_SHADER_VALIDATION_ABORT_ON_FAULT=1 \
ctest --test-dir build/pre-gpu-metal --output-on-failure \
  -R '^getnative_metal_tests$' \
  --output-log artifacts/pre-gpu-foundation/verification/macos-arm64/metal-validation-ctest.log
if rg -q 'metal conformance tests skipped: no Metal device' \
  artifacts/pre-gpu-foundation/verification/macos-arm64/metal-validation-ctest.log; then
  exit 1
fi
```

The Metal log must contain a real device name and must not contain
`metal conformance tests skipped: no Metal device`. Validation settings follow
Apple's API Validation and Shader Validation runtime guidance:
<https://developer.apple.com/documentation/xcode/validating-your-apps-metal-api-usage>
and
<https://developer.apple.com/documentation/xcode/validating-your-apps-metal-shader-usage>.

### 12.3 Serialized benchmark commands and artifact paths

The commands below are ordered gate invocations, not one unconditional pasted
script. After each command, complete the section 12.1 disposition and retained-
artifact action before starting the next listed command. Therefore the second
M2 remedy sees the first remedy when it was kept, M3 sees the final retained M2
path, M4c sees mandatory M4b, M5 sees retained M4b/M4c, and each M6 item sees
every earlier kept item. A blocked gate terminates the sequence.

M0 baseline:

```sh
cmake -E make_directory artifacts/pre-gpu-foundation/m0/macos-arm64
build/pre-gpu-release/getnative_scan_pipeline_benchmark \
  --gate M0 --backend cpu --scenarios S1,S2,S3,S4,S5,S6,S7 \
  --cache both --samples 21 \
  --json-out artifacts/pre-gpu-foundation/m0/macos-arm64/cpu.json \
  --manifest-out artifacts/pre-gpu-foundation/m0/macos-arm64/cpu.sha256 --assert
build/pre-gpu-metal/getnative_scan_pipeline_benchmark \
  --gate M0 --backend metal --scenarios S4,S5,S6,S7 \
  --cache both --samples 21 \
  --json-out artifacts/pre-gpu-foundation/m0/macos-arm64/metal.json \
  --manifest-out artifacts/pre-gpu-foundation/m0/macos-arm64/metal.sha256 --assert
```

M2 and M3 CPU gates:

```sh
cmake -E make_directory artifacts/pre-gpu-foundation/m2/macos-arm64
cmake -E make_directory artifacts/pre-gpu-foundation/retained/macos-arm64
build/pre-gpu-release/getnative_scan_pipeline_benchmark \
  --gate M2 --backend cpu --scenarios S1,S2,S3,S4,S5,S6,S7 \
  --cache both --samples 21 \
  --baseline artifacts/pre-gpu-foundation/m0/macos-arm64/cpu.json \
  --json-out artifacts/pre-gpu-foundation/m2/macos-arm64/cpu.json \
  --manifest-out artifacts/pre-gpu-foundation/m2/macos-arm64/cpu.sha256 --assert
cmake -E copy_if_different \
  artifacts/pre-gpu-foundation/m2/macos-arm64/cpu.json \
  artifacts/pre-gpu-foundation/retained/macos-arm64/cpu.json
cmake -E copy_if_different \
  artifacts/pre-gpu-foundation/m2/macos-arm64/cpu.sha256 \
  artifacts/pre-gpu-foundation/retained/macos-arm64/cpu.sha256
build/pre-gpu-release/getnative_scan_pipeline_benchmark \
  --gate M2 --backend cpu --scenarios S1,S2,S3,S4,S5,S6,S7 \
  --cache both --samples 21 --experiment m2-scratch-reuse \
  --baseline artifacts/pre-gpu-foundation/m0/macos-arm64/cpu.json \
  --previous artifacts/pre-gpu-foundation/retained/macos-arm64/cpu.json \
  --json-out artifacts/pre-gpu-foundation/m2/macos-arm64/m2-scratch-reuse.json \
  --manifest-out artifacts/pre-gpu-foundation/m2/macos-arm64/m2-scratch-reuse.sha256 --assert
build/pre-gpu-release/getnative_scan_pipeline_benchmark \
  --gate M2 --backend cpu --scenarios S1,S2,S3,S4,S5,S6,S7 \
  --cache both --samples 21 --experiment m2-exact-reserve \
  --baseline artifacts/pre-gpu-foundation/m0/macos-arm64/cpu.json \
  --previous artifacts/pre-gpu-foundation/retained/macos-arm64/cpu.json \
  --json-out artifacts/pre-gpu-foundation/m2/macos-arm64/m2-exact-reserve.json \
  --manifest-out artifacts/pre-gpu-foundation/m2/macos-arm64/m2-exact-reserve.sha256 --assert
cmake -E make_directory artifacts/pre-gpu-foundation/m3/macos-arm64
build/pre-gpu-release/getnative_scan_pipeline_benchmark \
  --gate M3 --backend cpu --scenarios S1,S2,S3,S4,S5,S6,S7 \
  --cache both --samples 21 --reference-mode m2-direct \
  --baseline artifacts/pre-gpu-foundation/m0/macos-arm64/cpu.json \
  --previous artifacts/pre-gpu-foundation/retained/macos-arm64/cpu.json \
  --json-out artifacts/pre-gpu-foundation/m3/macos-arm64/cpu.json \
  --manifest-out artifacts/pre-gpu-foundation/m3/macos-arm64/cpu.sha256 --assert
```

M4a, mandatory M4b, and optional M4c Metal gates:

```sh
cmake -E make_directory artifacts/pre-gpu-foundation/m4a/macos-arm64
build/pre-gpu-release/getnative_gpu_batch_tests \
  --write-fixture-hashes artifacts/pre-gpu-foundation/m4a/macos-arm64/gpu-batch-v1.sha256
cmp engine/tests/fixtures/gpu_batch_v1.sha256 \
  artifacts/pre-gpu-foundation/m4a/macos-arm64/gpu-batch-v1.sha256
build/pre-gpu-metal/getnative_scan_pipeline_benchmark \
  --gate M4a --backend metal --scenarios S4,S5,S6,S7 \
  --cache both --samples 21 \
  --baseline artifacts/pre-gpu-foundation/m0/macos-arm64/metal.json \
  --json-out artifacts/pre-gpu-foundation/m4a/macos-arm64/metal.json \
  --manifest-out artifacts/pre-gpu-foundation/m4a/macos-arm64/metal.sha256 --assert
cmake -E copy_if_different \
  artifacts/pre-gpu-foundation/m4a/macos-arm64/metal.json \
  artifacts/pre-gpu-foundation/retained/macos-arm64/metal.json
cmake -E copy_if_different \
  artifacts/pre-gpu-foundation/m4a/macos-arm64/metal.sha256 \
  artifacts/pre-gpu-foundation/retained/macos-arm64/metal.sha256
cmake -E make_directory artifacts/pre-gpu-foundation/m4b/macos-arm64
build/pre-gpu-metal/getnative_scan_pipeline_benchmark \
  --gate M4b --backend metal --scenarios S4,S5,S6,S7 \
  --cache both --samples 21 \
  --baseline artifacts/pre-gpu-foundation/m0/macos-arm64/metal.json \
  --previous artifacts/pre-gpu-foundation/retained/macos-arm64/metal.json \
  --json-out artifacts/pre-gpu-foundation/m4b/macos-arm64/metal.json \
  --manifest-out artifacts/pre-gpu-foundation/m4b/macos-arm64/metal.sha256 --assert
cmake -E copy_if_different \
  artifacts/pre-gpu-foundation/m4b/macos-arm64/metal.json \
  artifacts/pre-gpu-foundation/retained/macos-arm64/metal.json
cmake -E copy_if_different \
  artifacts/pre-gpu-foundation/m4b/macos-arm64/metal.sha256 \
  artifacts/pre-gpu-foundation/retained/macos-arm64/metal.sha256
cmake -E make_directory artifacts/pre-gpu-foundation/m4c/macos-arm64
build/pre-gpu-release/getnative_gpu_batch_tests \
  --write-fixture-hashes artifacts/pre-gpu-foundation/m4c/macos-arm64/gpu-batch-v1-candidate.sha256
build/pre-gpu-metal/getnative_scan_pipeline_benchmark \
  --gate M4c --backend metal --scenarios S4,S5,S6,S7 \
  --cache both --samples 21 --reference-mode m4b-retained \
  --experiment unique-plan-dedup \
  --baseline artifacts/pre-gpu-foundation/m0/macos-arm64/metal.json \
  --previous artifacts/pre-gpu-foundation/retained/macos-arm64/metal.json \
  --json-out artifacts/pre-gpu-foundation/m4c/macos-arm64/candidate.json \
  --manifest-out artifacts/pre-gpu-foundation/m4c/macos-arm64/candidate.sha256 --assert
```

Only after `KEPT`, review/update the checked-in final-enabled-set golden, then
rebuild, rerun the full M4c evidence against the still-retained M4b predecessor,
and promote exactly. The final rerun must also return `KEPT`; any other valid
disposition takes the removal branch, while invalid/noisy evidence reruns
unchanged:

```sh
cmake --build build/pre-gpu-release --parallel
cmake --build build/pre-gpu-metal --parallel
build/pre-gpu-release/getnative_gpu_batch_tests \
  --write-fixture-hashes artifacts/pre-gpu-foundation/m4c/macos-arm64/gpu-batch-v1.sha256
cmp engine/tests/fixtures/gpu_batch_v1.sha256 \
  artifacts/pre-gpu-foundation/m4c/macos-arm64/gpu-batch-v1.sha256
build/pre-gpu-metal/getnative_scan_pipeline_benchmark \
  --gate M4c --backend metal --scenarios S4,S5,S6,S7 \
  --cache both --samples 21 --reference-mode m4b-retained \
  --experiment unique-plan-dedup \
  --baseline artifacts/pre-gpu-foundation/m0/macos-arm64/metal.json \
  --previous artifacts/pre-gpu-foundation/retained/macos-arm64/metal.json \
  --json-out artifacts/pre-gpu-foundation/m4c/macos-arm64/metal.json \
  --manifest-out artifacts/pre-gpu-foundation/m4c/macos-arm64/metal.sha256 --assert
cmake -E copy_if_different \
  artifacts/pre-gpu-foundation/m4c/macos-arm64/metal.json \
  artifacts/pre-gpu-foundation/retained/macos-arm64/metal.json
cmake -E copy_if_different \
  artifacts/pre-gpu-foundation/m4c/macos-arm64/metal.sha256 \
  artifacts/pre-gpu-foundation/retained/macos-arm64/metal.sha256
```

Only after `REMOVED_AFTER_TEST`, revert the M4c source/switch and run this exact
mandatory-predecessor refresh before M5:

```sh
cmake --build build/pre-gpu-release --parallel
cmake --build build/pre-gpu-metal --parallel
build/pre-gpu-release/getnative_gpu_batch_tests \
  --write-fixture-hashes artifacts/pre-gpu-foundation/m4c/macos-arm64/m4b-gpu-batch-v1.sha256
cmp engine/tests/fixtures/gpu_batch_v1.sha256 \
  artifacts/pre-gpu-foundation/m4c/macos-arm64/m4b-gpu-batch-v1.sha256
build/pre-gpu-metal/getnative_scan_pipeline_benchmark \
  --gate M4b --backend metal --scenarios S4,S5,S6,S7 \
  --cache both --samples 21 --reference-mode retained-predecessor-rerun \
  --baseline artifacts/pre-gpu-foundation/m0/macos-arm64/metal.json \
  --previous artifacts/pre-gpu-foundation/retained/macos-arm64/metal.json \
  --json-out artifacts/pre-gpu-foundation/m4c/macos-arm64/m4b-reference-rerun.json \
  --manifest-out artifacts/pre-gpu-foundation/m4c/macos-arm64/m4b-reference-rerun.sha256 \
  --assert
cmake -E copy_if_different \
  artifacts/pre-gpu-foundation/m4c/macos-arm64/m4b-reference-rerun.json \
  artifacts/pre-gpu-foundation/retained/macos-arm64/metal.json
cmake -E copy_if_different \
  artifacts/pre-gpu-foundation/m4c/macos-arm64/m4b-reference-rerun.sha256 \
  artifacts/pre-gpu-foundation/retained/macos-arm64/metal.sha256
```

M5, only when its trigger is true:

```sh
cmake -E make_directory artifacts/pre-gpu-foundation/m5/macos-arm64
build/pre-gpu-metal/getnative_scan_pipeline_benchmark \
  --gate M5 --backend metal --scenarios S4,S5,S6,S7 \
  --cache both --samples 21 --reference-mode phase-separated \
  --baseline artifacts/pre-gpu-foundation/m0/macos-arm64/metal.json \
  --previous artifacts/pre-gpu-foundation/retained/macos-arm64/metal.json \
  --json-out artifacts/pre-gpu-foundation/m5/macos-arm64/metal.json \
  --manifest-out artifacts/pre-gpu-foundation/m5/macos-arm64/metal.sha256 --assert
```

M6 runs one of these exact experiment ids at a time and writes to the matching
directory; a false trigger writes only its disposition JSON:

```sh
cmake -E make_directory artifacts/pre-gpu-foundation/m6/macos-arm64
build/pre-gpu-metal/getnative_scan_pipeline_benchmark \
  --gate M6 --backend metal --scenarios S7,S8 --cache both --samples 21 \
  --experiment horizontal-intermediate \
  --previous artifacts/pre-gpu-foundation/retained/macos-arm64/metal.json \
  --json-out artifacts/pre-gpu-foundation/m6/macos-arm64/horizontal-intermediate.json \
  --manifest-out artifacts/pre-gpu-foundation/m6/macos-arm64/horizontal-intermediate.sha256 --assert
build/pre-gpu-metal/getnative_scan_pipeline_benchmark \
  --gate M6 --backend metal --scenarios S7 --cache both --samples 21 \
  --experiment global-signature-bucketing \
  --previous artifacts/pre-gpu-foundation/retained/macos-arm64/metal.json \
  --json-out artifacts/pre-gpu-foundation/m6/macos-arm64/global-signature-bucketing.json \
  --manifest-out artifacts/pre-gpu-foundation/m6/macos-arm64/global-signature-bucketing.sha256 --assert
build/pre-gpu-metal/getnative_scan_pipeline_benchmark \
  --gate M6 --backend metal --scenarios S7,S8 --cache both --samples 21 \
  --experiment gpu-plan-arena \
  --previous artifacts/pre-gpu-foundation/retained/macos-arm64/metal.json \
  --json-out artifacts/pre-gpu-foundation/m6/macos-arm64/gpu-plan-arena.json \
  --manifest-out artifacts/pre-gpu-foundation/m6/macos-arm64/gpu-plan-arena.sha256 --assert
```

M7 final reports use the retained final path and the frozen M0 baseline:

```sh
cmake -E make_directory artifacts/pre-gpu-foundation/m7/macos-arm64
build/pre-gpu-release/getnative_scan_pipeline_benchmark \
  --gate M7 --backend cpu --scenarios S4 --cache both --samples 21 \
  --baseline artifacts/pre-gpu-foundation/m0/macos-arm64/cpu.json \
  --previous artifacts/pre-gpu-foundation/retained/macos-arm64/cpu.json \
  --json-out artifacts/pre-gpu-foundation/m7/macos-arm64/cpu.json \
  --manifest-out artifacts/pre-gpu-foundation/m7/macos-arm64/cpu.sha256 --assert
build/pre-gpu-metal/getnative_scan_pipeline_benchmark \
  --gate M7 --backend metal --scenarios S4,S5,S6,S7 \
  --cache both --samples 21 \
  --baseline artifacts/pre-gpu-foundation/m0/macos-arm64/metal.json \
  --previous artifacts/pre-gpu-foundation/retained/macos-arm64/metal.json \
  --json-out artifacts/pre-gpu-foundation/m7/macos-arm64/metal.json \
  --manifest-out artifacts/pre-gpu-foundation/m7/macos-arm64/metal.sha256 --assert
```

### 12.4 Command-to-gate mapping

| Milestone | Required executable evidence |
| --- | --- |
| M0 | Release CPU/Metal builds; `scan_metrics` CTest; both M0 benchmark commands |
| M1 | `getnative_axis_plan_cache_tests` under release, ASan+UBSan, and TSan |
| M2 | `getnative_axis_planner_tests` under release/sanitizers plus M2 benchmark; remediation commands reuse `--gate M2` with `--experiment m2-scratch-reuse` or `m2-exact-reserve` |
| M3 | `getnative_scan_batch_tests` under release/sanitizers plus M3 paired benchmark |
| M4a | `getnative_gpu_batch_tests`, upstream conformance, Metal validation CTest, M4a benchmark, and fixture-hash output |
| M4b | Byte-identical GPU-batch tests, mid-flight stop/cancellation integration, Metal validation CTest, M4b benchmark, and mandatory retained-artifact promotion |
| M4c | Required one-variant evaluation: shared-plan semantic tests, Metal validation/cancellation regression, paired M4c benchmark, and exact keep/remove plus M4b-rerun disposition |
| M5 | If triggered, `getnative_scan_pipeline_tests` under release/sanitizers and M5 benchmark; otherwise trigger-disposition JSON |
| M6 | Section 11 tests and one exact experiment command per item; every item reads the retained predecessor, kept items promote it, and deleted items rerun it unchanged |
| M7 | Full release/sanitizer/Metal commands, M7 CPU/Metal reports, and the Windows hard lane below |

Every CTest command must exit 0 with zero failed tests. Sanitizer logs must have
zero reports. Benchmark commands must exit 0 and emit the named JSON plus
SHA-256 manifest; a missing artifact or manifest mismatch fails the gate.

### 12.5 Windows hard lane

The required lane id is `windows-x64-msvc-pre-gpu`. It runs on a real Windows
Server 2022 or Windows 11 x64 host from Visual Studio 2022 Developer PowerShell,
with CUDA and Vulkan SDKs absent. It executes exactly:

```powershell
$ErrorActionPreference = 'Stop'
cmake -E make_directory artifacts/pre-gpu-foundation/m7/windows-x64-msvc
cmake -S engine -B build/pre-gpu-win -G "Visual Studio 17 2022" -A x64 `
  -DGETNATIVE_ENABLE_METAL=OFF `
  -DGETNATIVE_BUILD_UPSTREAM_CONFORMANCE=OFF `
  -DGETNATIVE_WARNINGS_AS_ERRORS=ON `
  -DGETNATIVE_SANITIZER=none
if ($LASTEXITCODE -ne 0) { throw "Windows configure failed" }
cmake --build build/pre-gpu-win --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "Windows build failed" }
ctest --test-dir build/pre-gpu-win -C Release --output-on-failure `
  --output-log artifacts/pre-gpu-foundation/m7/windows-x64-msvc/ctest.log
if ($LASTEXITCODE -ne 0) { throw "Windows CTest failed" }
& build/pre-gpu-win/Release/getnative_gpu_batch_tests.exe `
  --write-fixture-hashes artifacts/pre-gpu-foundation/m7/windows-x64-msvc/gpu-batch-v1.sha256
if ($LASTEXITCODE -ne 0) { throw "Windows fixture hash generation failed" }
$hashDiff = Compare-Object `
  (Get-Content engine/tests/fixtures/gpu_batch_v1.sha256) `
  (Get-Content artifacts/pre-gpu-foundation/m7/windows-x64-msvc/gpu-batch-v1.sha256)
if ($hashDiff) { throw "Windows GPU batch fixture hashes differ" }
& build/pre-gpu-win/Release/getnative-engine.exe capabilities |
  Set-Content artifacts/pre-gpu-foundation/m7/windows-x64-msvc/capabilities.json
if ($LASTEXITCODE -ne 0) { throw "Windows CPU-only startup failed" }
$capabilities = Get-Content `
  artifacts/pre-gpu-foundation/m7/windows-x64-msvc/capabilities.json | ConvertFrom-Json
$cpu = $capabilities.backends | Where-Object { $_.id -eq 'cpu' }
$metal = $capabilities.backends | Where-Object { $_.id -eq 'metal' }
$cuda = $capabilities.backends | Where-Object { $_.id -eq 'cuda' }
if (-not $cpu.compiled -or $metal.compiled -or $cuda.compiled) {
  throw "Windows capabilities are not CPU-only"
}
dumpbin /DEPENDENTS build/pre-gpu-win/Release/getnative-engine.exe |
  Set-Content artifacts/pre-gpu-foundation/m7/windows-x64-msvc/imports.txt
if ($LASTEXITCODE -ne 0) { throw "dumpbin dependency inspection failed" }
$forbidden = Select-String `
  -Path artifacts/pre-gpu-foundation/m7/windows-x64-msvc/imports.txt `
  -Pattern 'nvcuda\.dll|cudart64_|vulkan-1\.dll'
if ($forbidden) { throw "CPU-only executable imports an optional GPU runtime" }
$compiler = Get-Command cl.exe -ErrorAction Stop
$compiler.FileVersionInfo | Format-List * |
  Set-Content artifacts/pre-gpu-foundation/m7/windows-x64-msvc/compiler.txt
if (-not $compiler.FileVersionInfo.FileVersion) { throw "MSVC version capture failed" }
cmake --version | Set-Content artifacts/pre-gpu-foundation/m7/windows-x64-msvc/cmake.txt
if ($LASTEXITCODE -ne 0) { throw "CMake version capture failed" }
Get-FileHash build/pre-gpu-win/Release/getnative-engine.exe -Algorithm SHA256 |
  Format-List | Set-Content artifacts/pre-gpu-foundation/m7/windows-x64-msvc/engine.sha256
```

The artifact bundle must contain all named files plus the Windows binary
SHA-256. CPU-only ABI/hash evidence is a hard M7 prerequisite. Missing/red
real-Windows evidence yields `BLOCKED_WINDOWS_EVIDENCE`; it is not a residual
risk and CUDA/Vulkan backend implementation cannot start. Only CUDA/Vulkan
runtime correctness/performance remains expectedly absent at this stage.

## 13. Milestone exit matrix

| Gate | M0 | M1 | M2 | M3 | M4a | M4b | M4c | M5 if used | M7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Full benchmark schema/baseline | required | unchanged | compare | compare | compare | compare | compare | compare | archive |
| AxisPlan bytes exact | baseline | required | required | required | required | required | required | required | required |
| Single physical build/key | observe gap | required | required | required | required | required | required | required | required |
| Stable CPU results/order | baseline | unchanged | required | required | required | required | required | required | required |
| M3 CPU/full-wall regression <=5% | baseline | n/a | n/a | required | retain | retain | retain | retain | required |
| Planner >=4x S4 | baseline | n/a | required | retain | retain | retain | retain | retain | required |
| Planner RSS <512 MiB | baseline | required | required | required | required | required | required | required | required |
| GPU POD/packing CPU-only | n/a | n/a | n/a | inputs ready | byte parity | byte parity retained | semantic dedup or removal | retained contract | required |
| Metal tolerance/top-k valley | baseline | unchanged | unchanged | unchanged | required | required | required | required | required |
| Metal exec regression <=5% | baseline | n/a | n/a | n/a | required | required | required | required | required |
| Mid-flight cancellation <=2 tiles | baseline | n/a | n/a | n/a | observe | required | retain | required | required |
| Explicit GPU bytes <2 GiB | baseline | n/a | n/a | n/a | required | required | required | required | required |
| Full-wall calibrated saving | baseline/freeze formula | measure | paired gate | paired gate | measure | observe only; no removal | keep/remove | keep/skip | final pass or verifier disposition |
| ASan/UBSan | baseline | required | required | required | required | required | required | required | required |
| TSan concurrency focus | n/a | required | required | required | required | required | required | required | required |
| CPU-only Windows readiness | lane defined | unchanged | unchanged | unchanged | mac golden frozen | unchanged | unchanged | unchanged | hard real-host gate |

## 14. Completion evidence package

M7 must archive or link:

- exact build/configure/test commands and exit results;
- baseline and final JSON reports with >=21-sample performance arrays, p95, and
  median absolute deviation;
- raw paired pack/upload calibration samples, direct stage totals, frozen
  `addressable_stage_budget`, and explicit measurement assumptions/limitations;
- SHA-256 source/CMake/binary/metallib/report manifests;
- host/compiler/device/driver/flags;
- exactness fixture result and upstream conformance result;
- cache single-flight/build-count evidence;
- ASan/UBSan and TSan summaries;
- Metal conformance, per-candidate metric, argmin, top-k local valley,
  mid-flight cancellation, workspace, and working-set evidence;
- the mandatory M4b cancellation checkpoint plus keep/remove decisions for M4c,
  M5, and every M6 item;
- updated architecture and Windows handover;
- the green `windows-x64-msvc-pre-gpu` CPU-only CTest, capability, import,
  binary-hash, and final-enabled-set cross-platform fixture-hash artifact bundle;
- residual risks other than the now-proven Windows CPU-only boundary.

The absence of CUDA/Vulkan runtime evidence is expected at this stage and must
be stated explicitly. This plan proves readiness to begin those backends; it
does not claim either backend exists.
