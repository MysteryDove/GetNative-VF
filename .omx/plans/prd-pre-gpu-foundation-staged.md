# PRD: Staged Pre-GPU Foundation

## Status

- Workflow: non-interactive `$ralplan`, deliberate but intentionally bounded
- Current revision: staged revision 3, submitted for Architect review 8
- Current execution scope: Stage 0 measurement plus Stage 1 batch planning only
- Companion test spec: `.omx/plans/test-spec-pre-gpu-foundation-stage-1.md`
- Supersedes the all-phases exploration draft in
  `.omx/plans/prd-pre-gpu-scan-foundation.md`
- No source implementation is authorized by this planning run

## 1. Outcome

Measure before deciding. First extend the existing benchmarks so planner time is
visible. Add bounded parallel construction of independent unique AxisPlans only
when the Stage 0 planner share passes its proceed gate; every individual plan and
LDLT remains serial.

Stop after Stage 1, compare fresh benchmark evidence, and write the next small
plan from those results. Do not pre-design the full scheduler, GPU packing ABI,
cache lifetime system, or pipeline before the measured bottleneck is known.

## 2. Current Evidence

Fresh one-shot/smoke runs on the Apple M4 Max on 2026-07-30:

| Existing benchmark | Result | What it proves |
| --- | --- | --- |
| `getnative_core_benchmark --assert` | AxisPlan `80.992 us`; CPU batch32 `9.335 ms`; `3427.944 candidates/s` | Single-plan micro/smoke anchor, not the Stage 0 decision population |
| `getnative_metal_benchmark --full --assert` | CPU `1538.473 ms`; Metal `111.097 ms`; `13.848x`; max error `5.61e-8`; valley distance `0` | One-shot GPU execution/correctness anchor, not a 5% comparison median |
| Metal peak memory | workspace `168.750 MiB`; working set `252.230 MiB` | Current execution memory guardrail |
| README verification record | five-run Metal median `125.409 ms` on the same host class | Historical population with different sampling; never mixed with the one-shot result |

The Metal benchmark builds all candidate plans before starting its CPU and Metal
timers (`engine/bench/metal_benchmark.cpp:115-144`). It is therefore a valid
execution-stage sanity anchor, but not yet a statistical planner or
phase-separated-total baseline. Stage 0 creates the decision baseline from fresh
21-sample measurements; historical numbers never drive the 5% gate.

## 3. RALPLAN-DR

### Principles

1. Measure one boundary, change one boundary, then measure again.
2. Parallelize independent unique plans, never the arithmetic inside one plan.
3. Preserve exact plan bytes and result order before accepting speed.
4. Stop when evidence does not justify the next layer.

### Decision drivers

1. Reduce real scan wall time, not only a microbenchmark.
2. Keep the first change small enough to revert or refine after one benchmark.
3. Avoid freezing Metal-specific policy into CUDA/Vulkan host contracts early.

### Options

#### Option A: staged batch planner, then reassess (selected)

Expose planner time, add one bounded unique-plan batch path, and stop for a data
review. This directly tests the current hypothesis with limited surface area.

#### Option B: implement the full host scheduler and portable GPU packing now

This may reduce later backend duplication, but it commits to lifetimes, error
ABIs, artifact workflows, packing layout, and overlap policy before the first
planner result exists. It is deferred, not rejected permanently.

#### Option C: start CUDA/Vulkan immediately

This minimizes host work now, but carries a known serial planner into each new
backend and leaves full-scan timing opaque. It becomes the preferred next step if
Stage 0 shows planner work is not material to the measured phase-separated total.

## 4. Current Scope

### Stage 0: make the existing benchmark decision-capable

Modify only the existing benchmark surfaces:

- `engine/bench/core_benchmark.cpp`
- `engine/bench/metal_benchmark.cpp`
- `engine/CMakeLists.txt` only if needed for the focused test/benchmark target

Add:

- precomputed active values and candidate ids outside every measured planner
  mode, so request materialization is identical;
- `plan_ms`: start before exact-key/dedup/cache lookup and stop only after
  request-ordered plan references are attached to candidates;
- existing `cpu_ms` and `metal_ms` unchanged as execution-only metrics;
- `cpu_total_ms = plan_ms + cpu_ms` and
  `metal_total_ms = plan_ms + metal_ms` for the current phase-separated path;
- fresh planner/cache state for every cold measured sample;
- repeat/sample count, raw values, median, MAD, and a small JSON output;
- build/compiler/host fields and the exact benchmark arguments.

Stage 0 runs serial planning for 21 samples. Proceed to Stage 1 only when the
median per-sample ratio `plan_ms / metal_total_ms >= 0.10`. Otherwise stop host
planner work and record immediate CUDA/Vulkan planning as the next direction.

Each run writes under a unique UTC directory. The benchmark fails if the final
JSON exists, writes and flushes a sibling temporary file, then atomically renames
it once. The output records executable/source identity and the synthetic fixture
generator identity. There is no mutable `latest` pointer, evidence database, or
promotion protocol.

### Stage 1: bounded unique-plan batch construction

Expected files:

- `engine/src/planner/axis_plan.cpp`
- New private `engine/src/planner/axis_plan_key.hpp`
- New private `engine/src/planner/axis_planner.hpp`
- New `engine/src/planner/axis_planner.cpp`
- New `engine/tests/axis_planner_test.cpp`
- `engine/bench/core_benchmark.cpp`
- `engine/bench/metal_benchmark.cpp`
- `engine/CMakeLists.txt`

Private, non-installed API shape, subject to normal implementation spelling:

```cpp
struct AxisPlanBatchOptions {
    std::size_t worker_count = 0; // bounded auto mode when zero
};

struct AxisPlanBatchResult {
    std::vector<std::shared_ptr<const AxisPlan>> plans; // request order
    std::size_t unique_key_count = 0;
    std::size_t physical_build_count = 0;
    std::size_t peak_active_builds = 0;
    std::size_t effective_worker_count = 0;
};

[[nodiscard]] AxisPlanBatchResult build_axis_plans(
    std::span<const AxisPlanRequest> requests,
    AxisPlanBatchOptions options = {});
```

Required behavior:

1. Move the existing private `PlanKey`, hash, and exact-bit `plan_key()` logic to
   the private shared key header without changing any field or bit semantics;
   both `AxisPlanCache` and the batch helper reuse it.
2. Deduplicate in stable first-occurrence order.
3. Empty input reports zero effective workers. For non-empty input, auto workers
   are `min(unique_count, max(1, min(hardware_concurrency, 8)))`; explicit
   workers clamp to `[1, unique_count]`.
4. Use and report one effective worker when there are at most two unique
   requests or the bounded worker result is one; otherwise report the actual
   per-call worker count.
5. Otherwise use one per-call `jthread` group plus an atomic work cursor, matching
   the existing CPU batch join-before-rethrow pattern. Each task calls unchanged
   serial `build_axis_plan()` into a private unique-key slot.
6. After failure observation, claim no new key. Join every worker, return no
   batch result, and rethrow the captured failure with the lowest stable
   first-occurrence key index. Successfully built private slots are not published
   to `AxisPlanCache`.
7. On complete success, scatter shared plan pointers back to request order and
   attach them to candidates before stopping `plan_ms`.
8. Finish planning before CPU/Metal candidate execution; no nested pools or
   planner/GPU overlap in Stage 1.
9. Wire only the benchmark pre-execution orchestration through this helper.
   `metal_backend.mm`, `getnative.metal`, public Metal headers, and Metal engine
   interfaces remain byte-for-byte untouched.

Stage 1 does not modify `AxisPlanCache` publication behavior and does not add a
persistent planner service, cross-call single-flight, LRU eviction, public header,
public scan error ABI, cancellation framework, portable GPU packing, or
production CLI scheduler. Those are possible later directions, not hidden
requirements of this stage.

## 5. Stage 1 Acceptance Gate

Correctness is mandatory:

- Every plan field/vector is byte-identical to serial `build_axis_plan()` for the
  existing fixture matrix, including Spline16 and non-default bicubic B/C.
- Output pointer order matches request order; repeated keys share a pointer.
- One batch physically builds each unique exact-bit key once.
- Worker concurrency never exceeds the requested/auto cap.
- Existing CPU, upstream conformance, Metal conformance, tolerance, valley, and
  memory assertions remain green.

Performance uses 7 paired samples while iterating and 21 paired samples for the
decision. Serial and batch modes run in the same final binary with alternating
pair order and identical precomputed requests. Define
`delta_i = (batch_i - serial_i) / serial_i`; a normalized paired-delta
`MAD > 0.025` yields `NO_DECISION_NOISY` and reruns unchanged.

- The 1000-unique cold plan-stage paired median must improve at least `2x` versus
  the same-binary serial mode.
- Batch sizes 1 and 2 must stay within `max(10%, 10 us)` of the serial path.
- Metal execution-only median must not regress more than 5%.
- Adopt Stage 1 only when the paired `metal_total_ms` median improves at least 5%
  and paired-delta MAD is at most 0.025.

If correctness, TSan, variance, and execution regression are green, revise once
only when exactly one performance gate misses within the companion test spec's
bounded near-miss band and profiling names one local cause. Rerun the unchanged
gate once. Otherwise, including a miss outside that band, unstable variance
after an unchanged rerun, correctness failure, or material execution regression,
remove the batch path and retain only the immutable evidence. No result starts a
broader scheduler automatically.

## 6. Evidence-Driven Next Directions

These are directions only. Each requires a new small plan after Stage 1 evidence:

| Stage 1 evidence | Candidate next direction |
| --- | --- |
| Planner remains at least 10% of phase-separated total or repeated calls show setup overhead | Persistent worker reuse, then cross-call single-flight if duplicate builds are observed |
| Host pack/upload is at least 10% of Metal total | Extract only the measured portable packing path; evaluate shared-plan dedup |
| Peak plan memory grows materially with scan length | Add bounded cache/sliced preparation before more concurrency |
| Queueing/cancellation latency is observed as a product problem | Add backend-local queue/cancellation hardening |
| No host stage is material after Stage 1 | Stop host-foundation work and begin the selected CUDA/Vulkan milestone |

No direction is pre-approved merely because it appears in this table.

## 7. Pre-Mortem

1. **Wrong timer boundary:** planner looks fast because construction remains
   outside the timer. Prevent with explicit `plan_ms` and `total_ms` around the
   real benchmark candidate loop.
2. **Parallel overhead erases the gain:** small batches slow down or too many
   workers contend. Prevent with 1/2/32/1000 cases and a bounded auto worker cap.
3. **Concurrency changes numerical identity:** plan construction order leaks into
   output or failure selection. Prevent with byte-exact plan comparisons, stable
   scatter, and ThreadSanitizer on the focused batch planner test.

## 8. ADR

### Decision

Use the existing core and Metal benchmarks as sanity anchors, create a fresh
Stage 0 serial decision baseline, implement private bounded unique-plan batch
construction only when planner share is at least 10%, and stop for a paired
benchmark-based direction review.

### Drivers

- Real full-wall impact.
- Small reversible change.
- Exact numerical compatibility.

### Alternatives considered

- Full host foundation now: deferred as premature.
- Immediate CUDA/Vulkan: selected next if Stage 1 shows host planning is minor.

### Consequences

- Later GPU host contracts are not frozen yet.
- The first implementation is deliberately smaller than the superseded draft.
- A new plan is required before any Stage 2 direction begins.

### Follow-ups

- Run Stage 0 and archive the exact immutable output.
- Implement Stage 1 only if the Stage 0 proceed gate passes.
- Produce a short result table and choose stop, revise once, or create the next
  stage plan.

## 9. Execution Staffing and Stop Condition

Relevant native roles: `executor`, `test-engineer`, `verifier`, `architect`, and
`critic`.

Stage 0/1 is small enough for one implementation owner plus one independent
verifier. Do not start multiple write lanes on `axis_plan.cpp`, benchmark files,
or CMake. `$performance-goal` is the appropriate durable follow-up if goal-mode
tracking is desired; `$ultragoal` is the general alternative. `$team` is not
recommended until a later stage has genuinely independent file ownership.

Planning stops after a fresh native Architect review and subsequent native
Critic approval of this staged scope. Execution stops after the Stage 1 evidence
table and recommendation; it does not auto-continue into Stage 2.

## 10. Staged Revision Changelog

- Replaced the all-phases design with one detailed measurement/implementation
  stage and benchmark-triggered future directions.
- Adopted the existing core/Metal benchmarks as separate planner/CPU and GPU
  execution baselines.
- Removed current requirements for a persistent service, artifact promotion
  system, Metal TSan lane, public error ABI, portable packing, and M4-M7 design.
- Applied staged Architect review 1: made Stage 1 conditional; froze paired timer
  boundaries and noise handling; kept the helper private and cache-independent;
  defined bounded workers, transactional failure, immutable UTC outputs, and one
  unambiguous adopt/revise/revert stop rule.
- Applied staged Architect review 2: defined the paired small-batch overhead
  formula and made revert the exhaustive fallback after the one bounded revision.
