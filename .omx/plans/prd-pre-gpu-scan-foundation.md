# PRD: Pre-GPU Scan Engine Foundation

## Document status

- Workflow: non-interactive `$ralplan`, deliberate planning depth
- Status: Superseded by `prd-pre-gpu-foundation-staged.md`; retained as an exploration draft only
- Context snapshot:
  `.omx/context/pre-gpu-scan-foundation-20260730T134813Z.md`
- Companion test specification:
  `.omx/plans/test-spec-pre-gpu-scan-foundation.md`
- Scope boundary: planning only; no CUDA or Vulkan implementation

## 1. Outcome

Before CUDA and Vulkan backend work starts, turn the current collection of a
serial AxisPlan builder, a parallel CPU candidate loop, and a Metal-specific
batch implementation into a measured scan-engine foundation with four stable
layers:

1. A deterministic, bounded, single-flight AxisPlan service.
2. A unique-key batch planner that uses coarse-grained CPU parallelism while
   each individual plan, including its LDLT, remains single-threaded.
3. A backend-neutral, bounded prepared-scan slice with stable global candidate
   indexes, local plan indexes, explicit cancellation, resource budgets, and
   result scatter.
4. A pure C++ GPU batch/packing contract adopted by Metal and reusable without
   redesign by CUDA and Vulkan.

The result must improve cold scan wall time, preserve exact CPU plan bytes and
results, avoid nested oversubscription, and expose enough stage evidence to
decide whether pipeline overlap and later micro-optimizations are justified.

## 2. Current state

- AxisPlan construction and banded LDLT are serial
  (`engine/src/planner/axis_plan.cpp:320-460`).
- The cache is thread-safe for publication but not single-flight
  (`engine/src/planner/axis_plan.cpp:515-534`).
- Callers still construct candidate plans serially
  (`engine/bench/metal_benchmark.cpp:115-128`).
- CPU candidate execution is already parallel and stable-order
  (`engine/src/backend/cpu/cpu_analysis.cpp:413-473`).
- Metal already has adjacent-signature tiling, workspace limits, queued tiles,
  and result-order preservation (`engine/src/backend/metal/metal_backend.mm:518-978`).
- Metal packing copies full coefficient arrays per candidate and keeps its POD,
  validation, tiling, and partial merge contract private to Objective-C++
  (`engine/src/backend/metal/metal_backend.mm:33-377`).
- The current Metal benchmark excludes planner time from both reported backend
  times (`engine/bench/metal_benchmark.cpp:115-144`).

## 3. Scope

### 3.1 P0: required before CUDA/Vulkan

- Full cold/warm scan benchmarking and structured stage counters.
- Crash-consistent retained-evidence promotion and restart recovery.
- Canonical coefficient-key identity.
- A public scan error taxonomy with stable diagnostics and retry policy.
- Single-flight, bounded, session-owned AxisPlan caching.
- Unique-key batch planning with a fixed worker cap and memory permits.
- Bounded, phase-separated prepared-scan slices with stable global indexes and
  deterministic result scattering.
- A pure C++ GPU batch layout, tiling, packing, validation, and partial merge
  implementation.
- Metal migration to that common contract with no numerical or performance
  regression.
- Mandatory backend-local queue/cancellation hardening with an independently
  retained evidence checkpoint.
- Evaluation of unique-plan coefficient deduplication as a removable overlay.
- Contract freeze with CPU-only and Metal evidence.

### 3.2 P1: required only when Phase 0 evidence justifies it

- A bounded two-window producer/consumer pipeline that prepares and packs the
  next GPU submission window while the current window executes.
- Allocator arenas beyond the bounded M2 remedies, or stable global signature
  bucketing when stage timings identify them as material bottlenecks.
- GPU-resident plan reuse across frames/jobs when real repeated-frame workloads
  prove upload or packing is material.

P1 decisions are made at explicit profiling gates. The API must remain
streaming-compatible even when the first implementation stays phase-separated.

### 3.3 P2: explicit non-goals for this program

- Parallelizing any stage inside one AxisPlan, including weight generation,
  band construction, and banded LDLT.
- Nested candidate/axis and intra-plan parallel regions.
- Changing Float64 planner order, Float32 solve order, strict threshold order,
  or fixed partial-merge order.
- Adaptive/coarse-to-fine candidate search in strict compatibility modes.
- Disk-persistent plan caches or unbounded process-global caches.
- A generic virtual backend framework not required by the shared data contract.
- FP16, fast math, relaxed precision, or a new solver.
- CUDA kernels, Vulkan shaders, driver loaders, packaging, or runtime claims.
- CLI/Tauri `analyze` wiring; it consumes the final contract later.

## 4. RALPLAN-DR

### 4.1 Principles

1. Preserve mathematical identity before pursuing throughput.
2. Parallelize independent unique plans, not the dependency chain inside one
   plan.
3. Share immutable data contracts and pure transformations, not device policy.
4. Bound every form of concurrency, cache residency, scratch memory, queueing,
   and GPU working memory.
5. Require end-to-end evidence; a faster isolated planner or kernel is not a
   faster scan unless full wall time improves.

### 4.2 Decision drivers

1. CUDA and Vulkan must reuse one already-proven host plan/packing contract.
2. Strict AxisPlan bytes, CPU results, candidate order, and GPU valley behavior
   cannot regress.
3. The 1000-candidate scan workload must use available CPU cores without
   starving GPU submission, decoding, or the application thread.

### 4.3 Viable options

#### Option A: planner-only patch

Add `get_or_build_many()` and a thread pool around current AxisPlan creation;
leave Metal packing and scheduling private.

Pros:

- Smallest diff and fastest initial planner speedup.
- Low immediate Metal regression risk.

Cons:

- CUDA and Vulkan would still duplicate private Metal validation, PODs, tiling,
  packing, workspace arithmetic, and merge behavior.
- No stable place for full-pipeline telemetry, cancellation, or later overlap.
- Likely forces a second interface migration during backend implementation.

#### Option B: layered measured foundation (selected)

Measure first, then add plan identity/cache, batch planning, prepared-scan data,
portable GPU packing, Metal adoption, and only evidence-triggered overlap.

Pros:

- Solves the current serial planning and duplicate-build problems.
- Gives CUDA/Vulkan a CPU-tested host contract before device code exists.
- Keeps device-specific execution policy in each backend.
- Allows every architectural step to be benchmarked and reverted independently.

Cons:

- More up-front work than a planner-only patch.
- Metal must migrate before the contract can be declared proven.
- Requires careful lifetime, cancellation, and byte-budget design.

#### Option C: full asynchronous backend framework now

Build a generic job DAG, virtual backend interface, persistent GPU plan cache,
global signature bucketing, and multi-device scheduling in one program.

Pros:

- Maximizes theoretical reuse and future flexibility.
- Could support cross-frame and multi-device pipelines from day one.

Cons:

- Commits to abstractions before CLI jobs, CUDA, Vulkan, and representative
  workloads exist.
- Greatly expands deadlock, cancellation, lifetime, and device-loss surfaces.
- Makes it difficult to attribute performance changes to one intervention.

### 4.4 Selection rationale

Option B is the only option that addresses current measured architectural
duplication without making unmeasured asynchronous policy part of the initial
contract. Option A is useful as an implementation step but insufficient as the
pre-CUDA/Vulkan stopping point. Option C remains invalid until the lower-level
contracts and production job semantics are proven.

## 5. Required invariants

### 5.1 Plan identity

- One schedulable plan task is one unique, cache-missing `AxisPlanKey`.
- Candidate id, candidate index, axis label, frame id, plane name, and backend
  are consumer/provenance fields, not key fields.
- Key fields are source size, destination size, exact relevant active-length and
  shift bits, canonical kernel type and only its coefficient-affecting
  parameters, and border mode.
- Bilinear and spline keys ignore irrelevant raw B/C/taps fields; bicubic keeps
  B/C and canonicalizes support/taps; Lanczos keeps taps and ignores B/C.
- Relevant floating-point fields retain exact bit patterns in P0. Merging `-0`
  and `+0` is allowed only after a byte-equality test proves identical plans.
- Future blur, post-convolution, coordinate mode, precision, or layout changes
  must extend the key before they can affect coefficients.

### 5.2 Numerical behavior

- `build_axis_plan()` remains the only coefficient generator.
- A task invokes one complete serial build. The current Float64 order through
  band formation, LDLT, and Float32 projection remains unchanged.
- Existing plan arrays compare bit-for-bit against the pre-change builder for
  the frozen conformance matrix.
- CPU scalar and batch metrics remain bit-identical.
- GPU strict acceptance remains
  `abs(gpu-cpu) <= max(1e-7, 5e-4*abs(cpu))`, valley step distance `<=1`, and
  stable candidate/result order.

### 5.3 Concurrency and lifetime

- Cache acquisition is one atomic disposition: `ready`, `existing_flight`, or
  `new_build_claim`. Only `new_build_claim` may become a planner task;
  `existing_flight` waits without consuming a worker slot.
- Equal concurrent misses in one cache generation start exactly one physical
  build; all waiters receive the same shared plan or the same exception.
- Different unique keys may build concurrently up to the configured worker and
  scratch-byte limits.
- The cache lock is never held while constructing a plan or waiting for one.
- A cancelled waiter stops waiting but does not cancel a shared build needed by
  other callers. Already-started plan builds may finish; no new work is issued.
- A build claim that is cancelled or abandoned before submission completes its
  flight with a typed terminal error and removes only its identity-matched
  in-flight entry.
- Failed builds are not resident and can be retried.
- `clear()` removes ready/current-flight indexes and advances a generation. A
  new caller starts or joins only the new generation. Earlier flights remain
  alive for existing holders, but completion is checked against generation and
  flight identity so an old builder cannot admit into or erase a newer same-key
  flight.
- Eviction removes cache ownership only; external `shared_ptr`s stay valid.
- P0 admits at most one `build_many()` call at a time per `AxisPlanService`.
  Concurrent callers wait stop-aware at a serialized batch-admission gate and
  acquire no cache claim until admitted. The cache claim protocol itself
  remains thread-safe and single-flight for concurrent direct cache users and
  future service policy.
- `request_stop()` is the thread-safe, idempotent transition from accepting to
  stopping. It rejects new calls, wakes admission/cache/queue waiters, abandons
  unsubmitted claims, and lets already-running serial plan builds reach their
  terminal publication.
- `shutdown()` is idempotent, includes `request_stop()`, and joins the fixed
  internal workers while the service object remains alive. It may run while a
  `build_many()` caller is unwinding, but it does not destroy shared service or
  per-call state.
- The owner must join every thread executing any public service member before
  beginning object destruction. The destructor may invoke `shutdown()` if
  needed, then destroys only terminal internal state; concurrent destruction
  with an active public member call is outside the contract and never used as
  a cancellation mechanism.

### 5.4 Resource policy

- No static process-global planner pool or cache.
- A scan/session owns its plan service, cache, and `ScanResourceBudget`.
- Worker policy is a versioned runtime option calibrated in M0, not a frozen
  ABI. Auto mode must remain within `[1, hardware_threads]` and leave measured
  headroom for submission/application work; exact reserve counts are retained
  only when benchmarks support them.
- Before any cache acquisition, the admitted P0 batch normalizes/deduplicates
  all requests and checked-sums request count, unique pending claims, and the
  distinct returned-plan storage upper bound. A request exceeding any batch or
  service ceiling fails without creating a flight.
- Since one batch is admitted per service, its pending-claim, active scratch,
  and active retained-plan permits are service-wide P0 ceilings. The bounded
  worker queue is not treated as a pending-claim or memory bound.
- A checked upper bound reserves output and scratch permits before each build.
  Internal planner temporaries allocate through a capped, counting scratch
  resource, so actual logical allocation cannot exceed the claim. Completed
  output is reconciled against the active batch retained-plan permit before
  publication/return. Process RSS remains an observed external gate because
  allocator overhead is not the logical budget.
- The active retained-plan permit ends when `build_many()` transfers its result
  to the caller. Caller-held `shared_ptr` storage after return is explicitly an
  outer slice/session responsibility: `PreparedScanSlice` counts distinct
  retained plan bytes against `maximum_host_plan_bytes_per_slice`, while ready
  cache ownership remains bounded independently by the cache limits.
- CPU backend planning and candidate execution are phase-separated in P0 and
  share the same total worker budget, so their parallel regions never nest.
- GPU paths reserve at least one CPU for submission/application work; the exact
  auto policy is retained only if Phase 0 benchmarks support it.
- Resident cache bytes use logical owned vector sizes plus object size. In-flight
  scratch, ready cache bytes, packed host bytes, queued plan bytes, GPU arena,
  and partials are reported separately.

### 5.5 Ordering and errors

- Each bounded slice indexes unique plans by first occurrence within the slice;
  its candidates retain global original input index and id.
- Adjacent compatible candidates form tiles in P0; global rebucketing is not
  permitted until profile evidence exists.
- Results are scattered by original input index, never by completion order.
- A full scan returns complete ordered results or `ScanError`; completed internal
  slices never become an unlabeled partial final success.
- Successful plans built before another key fails may remain cached.
- Every public planner, preparation, packing, and backend boundary maps internal
  exceptions to exactly one `ScanErrorCode`. Compatibility wrappers may retain
  standard exceptions only below that public boundary.
- The frozen codes are `cancelled`, `service_stopped`, `resource_exhausted`,
  `invalid_input`, `arithmetic_overflow`, `backend_device_failure`,
  `internal_build_failure`, and `batch_abandoned`.
- Every error carries `stage`, `backend`, optional normalized key, optional global
  candidate index, a retry policy, and a stable diagnostic message. Fields that
  do not apply are absent rather than filled with sentinel values.
- Retry policy is exact: `cancelled` permits a new call after the caller supplies
  a live stop token; `service_stopped` requires a new service; resource exhaustion
  requires a smaller request or larger approved budget; invalid input and
  arithmetic overflow never retry unchanged; backend device failure requires
  backend/device recreation; internal build failure and batch abandonment permit
  only an explicit new call and are never retried automatically.
- Equal-flight waiters receive the same immutable code, stage, backend, key,
  retry policy, and diagnostic payload. A cancelled waiter may instead return
  its own `cancelled` result without changing the shared flight.
- Boundary mapping is fixed: validation -> `invalid_input`; checked arithmetic or
  narrowing -> `arithmetic_overflow`; configured ceilings or bounded allocation
  failure -> `resource_exhausted`; caller stop -> `cancelled`; calls after service
  stop -> `service_stopped`; Metal command/device failure ->
  `backend_device_failure`; an unexpected builder exception ->
  `internal_build_failure`; and an unsubmitted claim abandoned because a sibling
  failed -> `batch_abandoned` while the batch returns the original first error.

### 5.6 Local-valley oracle

- Add one pure CPU helper used by both scan benchmarks and Metal conformance;
  neither test suite may carry a private interpretation of a valley.
- Input metrics must be finite. Empty input has no valley. Split the input into
  maximal contiguous plateaus using numeric equality (`-0 == +0`). A plateau
  `[a,b]` is a local valley when its value is strictly lower than each neighbor
  that exists. Thus a single endpoint may qualify against its only neighbor,
  and an all-equal non-empty input is one valley.
- Represent a qualifying plateau by its leftmost index `a`. Rank valleys by
  ascending metric and then ascending representative index; this defines equal
  metrics and the stable tie-break.
- For CPU/GPU top-k comparison, independently select the first
  `min(5, valley_count)` ranked valleys. The selected cardinalities must be
  equal. Sort their representative indexes ascending, pair by position, and
  require every absolute index distance to be `<=1`. This is the sole
  deterministic one-to-one matching rule; no many-to-one or rematching is
  permitted.

## 6. Proposed contracts

Names are implementation targets. Review may refine spelling, but execution
must not weaken their semantics.

### 6.1 Canonical key and cache

In `engine/include/getnative/axis_plan.hpp`:

```cpp
struct AxisPlanKey {
    std::int32_t source_size;
    std::int32_t destination_size;
    std::uint64_t active_length_bits;
    std::uint64_t shift_bits;
    KernelType kernel;
    std::uint64_t parameter_0_bits;
    std::uint64_t parameter_1_bits;
    std::int32_t taps;
    BorderMode border;
    friend bool operator==(const AxisPlanKey &, const AxisPlanKey &) = default;
};

[[nodiscard]] AxisPlanKey make_axis_plan_key(const AxisPlanRequest &);
[[nodiscard]] std::size_t axis_plan_storage_upper_bound(
    const AxisPlanRequest &);
[[nodiscard]] std::size_t axis_plan_storage_bytes(const AxisPlan &) noexcept;

struct AxisPlanCacheLimits {
    std::size_t maximum_entries = 1024;
    std::size_t maximum_resident_bytes = 256U * 1024U * 1024U;
};

struct AxisPlanCacheStats {
    std::uint64_t lookups;
    std::uint64_t ready_hits;
    std::uint64_t misses;
    std::uint64_t builds_started;
    std::uint64_t builds_completed;
    std::uint64_t build_failures;
    std::uint64_t in_flight_waits;
    std::uint64_t evictions;
    std::size_t resident_entries;
    std::size_t resident_bytes;
    std::size_t peak_resident_bytes;
};

enum class AxisPlanDisposition : std::uint8_t {
    ready,
    existing_flight,
    new_build_claim,
};
```

`AxisPlanCache` becomes bounded LRU for ready entries and keeps a separate
single-flight map for in-flight entries. An oversize completed plan is returned
to callers but not admitted to the resident LRU. Its internal atomic
`acquire(key)` result carries the disposition, cache generation, and unique
flight identity. A move-only RAII build claim has exactly three terminal
operations: publish a plan, publish an exception, or abandon before submission.
Completion removes/adopts only an identity-matched current-generation flight.
Wait handles own shared flight state and provide stop-aware waiting without a
planner worker. The compatibility `get_or_build()` resolves the same protocol
synchronously.

The numeric cache defaults above are provisional application policy and are
calibrated in M0; the frozen contract is that both ceilings are finite,
configurable, observable, and enforced.

### 6.2 Batch plan service

Add:

- `engine/include/getnative/scan_error.hpp`
- `engine/src/analysis/scan_error.cpp`
- `engine/include/getnative/resource_budget.hpp`
- `engine/src/scheduler/resource_budget.cpp`
- `engine/include/getnative/axis_planner.hpp`
- `engine/src/planner/axis_planner.cpp`

```cpp
enum class ScanErrorCode : std::uint8_t {
    cancelled,
    service_stopped,
    resource_exhausted,
    invalid_input,
    arithmetic_overflow,
    backend_device_failure,
    internal_build_failure,
    batch_abandoned,
};

enum class ScanStage : std::uint8_t {
    validation,
    keying,
    cache_wait,
    plan_build,
    preparation,
    packing,
    submission,
    execution,
    readback,
    merge,
};

enum class ScanBackend : std::uint8_t { host, cpu, metal, cuda, vulkan };

enum class ScanRetryPolicy : std::uint8_t {
    never,
    new_call,
    new_service,
    after_request_or_budget_change,
    after_backend_reset,
};

struct ScanErrorData {
    ScanErrorCode code;
    ScanStage stage;
    ScanBackend backend;
    std::optional<AxisPlanKey> key;
    std::optional<std::size_t> candidate_index;
    ScanRetryPolicy retry;
    std::string message;
};

class ScanError final : public std::runtime_error {
public:
    explicit ScanError(std::shared_ptr<const ScanErrorData>);
    [[nodiscard]] const ScanErrorData &data() const noexcept;
};

struct ScanResourceBudget {
    std::size_t total_cpu_workers;
    std::size_t maximum_planner_logical_bytes;
    std::size_t maximum_plan_requests_per_batch;
    std::size_t maximum_pending_plan_claims;
    std::size_t maximum_retained_plan_bytes_per_batch;
    AxisPlanCacheLimits cache;
    std::size_t maximum_candidates_per_slice;
    std::size_t maximum_host_plan_bytes_per_slice;
};

struct AxisPlanServiceOptions {
    std::size_t worker_count = 0;
    std::size_t serial_work_threshold = 0; // calibrated by Phase 0
    ScanResourceBudget budget;
};

struct AxisPlanBatchStats {
    std::size_t request_count;
    std::size_t unique_key_count;
    std::size_t ready_hit_count;
    std::size_t waited_key_count;
    std::size_t claimed_key_count;
    std::size_t built_key_count;
    std::size_t peak_active_builds;
    std::size_t peak_pending_claims;
    std::size_t peak_claimed_logical_bytes;
    std::size_t peak_actual_scratch_bytes;
    std::size_t peak_actual_output_bytes;
    std::size_t retained_plan_bytes;
    std::size_t peak_service_retained_plan_bytes;
    std::chrono::nanoseconds batch_admission_wait_time;
    std::chrono::nanoseconds wall_time;
};

struct AxisPlanBatch {
    std::vector<std::shared_ptr<const AxisPlan>> plans; // request order
    AxisPlanBatchStats stats;
};

class AxisPlanService {
public:
    explicit AxisPlanService(AxisPlanServiceOptions = {});
    ~AxisPlanService();
    AxisPlanService(const AxisPlanService &) = delete;
    AxisPlanService &operator=(const AxisPlanService &) = delete;
    [[nodiscard]] AxisPlanBatch build_many(
        std::span<const AxisPlanRequest>, std::stop_token = {});
    [[nodiscard]] AxisPlanCacheStats cache_stats() const;
    void clear_cache();
    void request_stop() noexcept;
    void shutdown();
};
```

Public entry points throw `ScanError` and never require callers to parse text or
backend-native error objects. Cache flights store one immutable
`shared_ptr<const ScanErrorData>` so equal-flight waiters observe identical
diagnostics. Internal `invalid_argument`, `length_error`, `bad_alloc`, unexpected
planner exceptions, and Metal command-buffer/device errors are mapped once at
the owning boundary according to section 5.5; already-typed errors pass through
unchanged. No automatic retry occurs inside the service or backend.

The service owns the cache, reusable fixed worker set, and bounded queue. It
serializes P0 `build_many()` admission. After admission it deduplicates requests,
rejects request-count, worst-case pending-claim, integer-overflow, or retained-
plan upper-bound excess before the first cache acquisition, then atomically
acquires every unique key. Preflight conservatively treats every unique key as a
possible new claim; runtime `peak_pending_claims` counts only nonterminal
`new_build_claim`s. The service submits only new claims and lets existing-flight
consumers wait outside the worker pool. Each task performs one complete AxisPlan
build with unchanged arithmetic order. Small batches use a calibrated serial
path but still resolve claims through the same cache protocol.

Internal temporary vectors use an `AxisPlanBuildScratch` backed by a capped,
counting `std::pmr::memory_resource`. The public serial builder remains a
wrapper around the same implementation and obtains a one-build scratch arena.
Tests must prove that allocator refactoring changes no bytes. A checked upper
bound is used for admission; actual scratch and output logical high-water values
are recorded and asserted not to exceed the claim. RSS is measured separately.

`request_stop()` and `shutdown()` follow the lifecycle in section 5.3. An
admission waiter observes its own stop token or service stop without acquiring
claims. The active call stops submitting at the first observed stop/failure,
terminally abandons every unsubmitted claim, waits for submitted work to reach a
stable terminal state, releases all active service permits, and then returns or
throws. `shutdown()` joins internal workers but leaves object/per-call storage
alive so active public calls can unwind; callers join those member-call threads
before the destructor begins. `cache_stats()` remains readable until that join;
`build_many()` and `clear_cache()` reject after service stopping begins.

Returned `AxisPlanBatch` storage is caller-owned. Its `retained_plan_bytes`
counts each distinct plan pointer once, regardless of request multiplicity. The
prepared-slice layer must carry this count into its own host-plan-byte permit,
so releasing the service call permit cannot make retained slice storage
unobservable or unbounded.

### 6.3 Bounded scan preparation

Add `engine/include/getnative/scan_batch.hpp` and
`engine/src/scheduler/scan_batch.cpp`. This is an indexed preparation layer,
not a production job scheduler or backend-polymorphism framework.

```cpp
inline constexpr std::uint32_t no_plan = UINT32_MAX;

struct CandidatePlanRequest {
    std::string id;
    std::optional<AxisPlanRequest> horizontal;
    std::optional<AxisPlanRequest> vertical;
    AnalysisAxes axes;
};

struct PreparedCandidate {
    std::size_t input_index;
    std::string id;
    std::uint32_t horizontal_plan = no_plan;
    std::uint32_t vertical_plan = no_plan;
    AnalysisAxes axes;
};

struct PreparedScanSlice {
    std::size_t input_begin;
    std::size_t input_end;
    std::vector<AxisPlanKey> plan_keys; // slice first-occurrence order
    std::vector<std::shared_ptr<const AxisPlan>> plans;
    std::vector<PreparedCandidate> candidates; // global input indexes
    AxisPlanBatchStats planner;
};
```

`ScanPreparationCursor` walks an input span in bounded, adjacent slices. It
validates axes/optional plans, flattens consumers, builds through the shared
`AxisPlanService`, creates a slice-local plan table, and returns stable global
indexes. P0 executes and scatters one complete slice before preparing the next,
so arbitrarily large scans do not require a materialized all-candidate batch.
A compatibility adapter materializes current `CandidateAnalysis` views for the
CPU and existing backend entry points. M5 may overlap two cursored slices, but
does not change their semantic representation.

`ScanResourceBudget` is configuration/data shared by the service and cursor,
not a process-global allocator. Backend device limits and submission policy
remain in Metal/CUDA/Vulkan option structs.

### 6.4 Portable GPU batch contract

Add `engine/include/getnative/gpu_batch.hpp` and
`engine/src/backend/gpu/gpu_batch.cpp` with no GPU SDK headers or libraries.

The module owns:

- `GpuBatchSemanticVersion` and `GpuPodVersion`; v1 is additive-versioned rather
  than treated as an eternal ABI.
- `GpuKernelShape`, `GpuTileSignature`, and checked shape classification.
- `alignas(16) GpuAxisPlanDescriptor` with the existing 64-byte layout.
- `GpuAnalysisJob` with the existing 40-byte layout.
- Workspace calculation for H, V, and both axes.
- Adjacent-signature, candidate-count, workspace, plan-byte, and 32-bit-offset
  bounded tiles/windows.
- `GpuPackedPlanTable`: flattened coefficient arrays with behavior-preserving
  per-candidate plan references in M4a/M4b. M4c may compact this table by stable
  first occurrence, but deduplication is not required by semantic-v1.
- Per-candidate descriptors that may reuse coefficient bases after M4c while
  always keeping distinct workspace bases, direction, vector count, and
  two-axis auxiliary fields.
- A stable result-index map and fixed-order Float32-partial-to-double p=1 merge.
- Checked arithmetic and validation currently private to Metal.

Suggested pure API:

```cpp
[[nodiscard]] GpuBatchLayout make_gpu_batch_layout(
    ConstImageView, const PreparedScanSlice &, const GpuBatchLimits &);

[[nodiscard]] GpuSubmissionWindow pack_gpu_submission_window(
    ConstImageView, const PreparedScanSlice &, const GpuBatchLayout &,
    std::size_t first_tile, std::size_t tile_count);

void merge_p1_partials(std::span<const float> partials,
                       std::span<const std::size_t> result_indexes,
                       std::size_t groups, double pixel_count,
                       std::span<CandidateResult> output);
```

Metal keeps queue creation, MTLBuffer policy, pipeline selection, command
encoding, submission, and device diagnostics. The common module must not own
Metal/CUDA/Vulkan handles or choose device-specific transfer policy.

Tile size, submission queue depth, worker policy, shared/private/device-local
transfer strategy, and backend-specific additive descriptor buffers are not
part of the frozen v1 semantic contract.

### 6.5 Shared local-valley oracle

Add `engine/include/getnative/scan_metrics.hpp` and
`engine/src/analysis/scan_metrics.cpp` as pure CPU code with no backend SDK
dependency:

```cpp
struct RankedLocalValley {
    std::size_t index;
    double metric;
};

[[nodiscard]] std::vector<RankedLocalValley> rank_local_valleys(
    std::span<const double> metrics);

[[nodiscard]] bool top_local_valleys_match(
    std::span<const double> reference,
    std::span<const double> actual,
    std::size_t maximum_count = 5,
    std::size_t maximum_index_distance = 1);
```

The implementation is exactly the plateau/ranking/matching algorithm in
section 5.6. It rejects nonfinite input and is the only oracle used by the
benchmark, CPU fixtures, and Metal conformance.

### 6.6 Telemetry

Add a versioned `ScanRunStats` data model under
`engine/include/getnative/scan_stats.hpp`. It records wall durations and byte
counts for:

1. request normalization/keying;
2. cache lookup/wait;
3. plan build;
4. prepared-scan assembly;
5. tile layout;
6. host pack;
7. upload/staging;
8. inverse/forward/metric execution;
9. readback;
10. host merge/scatter;
11. total wall time.

Counters include requests, unique plans, ready hits, waits, physical builds,
pending claims, active/returned distinct plan bytes, tiles, submission windows,
logical coefficient bytes, emitted coefficient bytes, uploaded bytes, peak
planner workers/scratch, cache residency, host packing bytes, GPU workspace,
and total explicit GPU working set.

Benchmarks emit versioned JSON plus the existing human-readable summary. Timing
is diagnostic and benchmark-facing; correctness never depends on clocks.

### 6.7 Performance comparison arithmetic

All threshold decisions use at least 21 measured samples, median, p95, and MAD.
The benchmark emits the raw samples and performs the arithmetic below; an
executor may not substitute a best run, mean, byte-scaled estimate, or pooled
workload average.

#### M3 paired regression gate

The M3 benchmark binary retains a benchmark-only `m2_direct` control path using
the M2 plan service plus the pre-slice CPU adapter. For every S1-S7 workload it
runs 21 alternating-order pairs of `m2_direct` and `m3_slice` in one process,
with identical input, worker policy, and limits. Cold pairs use separate fresh
service sessions for both sides; warm pairs use separate equivalently primed
sessions. Define, for both CPU execution and total wall:

```text
delta_i = m3_slice_i - m2_direct_i
ratio_i = delta_i / m2_direct_i
```

- Evidence is invalid when `MAD(ratio_i) > 0.02`; stabilize load/thermals and
  rerun the unchanged command. Evidence is also invalid if any
  `m2_direct_i <= 0`, because the paired ratio is then undefined. Noise never
  widens the threshold.
- S1-S2 pass when `median(delta_i) <= max(0.05 * median(m2_direct_i), 5 us)`.
- S3-S7 pass when `median(ratio_i) <= 0.05`.
- M0-to-M2 planner gain remains a separately reported comparison. It is never
  subtracted from M3 time or used to mask an M3 regression.
- On failure, M3 may evaluate at most two remediation variants limited to slice
  capacity and allocation reservation. Each variant reruns the full command.
  If neither passes, M3 is blocked and returns to architecture review; M4 does
  not start.

#### Final full-wall gate

Apply the final gate separately, without aggregation, to CPU/S4 and Metal/S4,
S5, S6, and S7. Metal cases exist only on the Apple validation host. For each
applicable backend/workload pair, let:

```text
B = M0 addressable_stage_budget
T0 = M0 total-wall median
Tf = final retained phase-separated/overlapped-path total-wall median
N = max(0.01 * T0, 3 * M0 total-wall MAD, 3 * final total-wall MAD)
target = min(0.20 * T0, 0.70 * B)
observed_saving = T0 - Tf
```

The measured-win gate passes only when `B > N` and
`observed_saving >= max(target, N)`. When `B <= N`, the result is
`NO_MEASURABLE_ADDRESSABLE_BUDGET`, not a zero-target pass and not a performance
claim. When `B > N` but saving is below the gate, the result is
`MEASURED_TARGET_MISS`.

Either final non-pass disposition requires a native performance-verifier
decision at M7 and cannot be presented as a successful optimization claim.
Mandatory M4b cancellation hardening is never removed by this performance
formula. Optional M4c/M5/M6 overlays already have independent predecessor-based
keep/remove gates; M7 verifies those decisions and reruns the final formula
after any overlay-specific removal required by its own gate. The stable
retained-predecessor artifact always names the exact executable under test. M7
may describe the contract as backend-ready only when all other gates pass, but
it must label the affected workload `no measured host-side win`.

## 7. Implementation milestones

### M0 - Freeze full-pipeline baseline and observability (P0)

Files:

- `engine/bench/core_benchmark.cpp`
- `engine/bench/metal_benchmark.cpp`
- New `engine/bench/scan_pipeline_benchmark.cpp`
- New `engine/include/getnative/scan_stats.hpp`
- New `engine/include/getnative/scan_metrics.hpp`
- New `engine/src/analysis/scan_metrics.cpp`
- New `engine/tests/scan_metrics_test.cpp`
- New `engine/tools/evidence_controller.cpp`
- New `engine/tests/evidence_controller_test.cpp`
- `engine/CMakeLists.txt`
- New `docs/performance/pre-gpu-foundation-baseline.md`

Work:

1. Add `GETNATIVE_WARNINGS_AS_ERRORS=ON|OFF` and one validated
   `GETNATIVE_SANITIZER=none|address-undefined|thread` CMake cache setting.
   Address+UB and Thread sanitizers are mutually exclusive; unsupported
   compiler/platform combinations fail configuration instead of silently
   disabling instrumentation.
2. Register stable CTest names for each new test target and implement the exact
   benchmark CLI consumed by the test-spec command matrix:
   `--gate`, `--backend`, `--scenarios`, `--cache`, `--samples`, `--json-out`,
   `--manifest-out`, optional `--baseline`, optional `--previous`, optional
   `--reference-mode`, optional `--experiment`, and `--assert`. `--previous`
   accepts only the authoritative retained `current.json` defined by the test
   spec, resolves its immutable bundle, verifies every recorded hash, and never
   guesses a previous milestone from a directory or milestone number.
3. Add `getnative_evidence_controller promote|verify|recover`. A promotion stages
   one immutable report/manifest bundle on the retained filesystem, fsyncs and
   verifies both files and their directory, renames the bundle into its final
   transaction-id path, then fsyncs and atomically replaces same-directory
   `current.json`. The pointer contains schema version, transaction id, backend,
   gate/experiment/disposition, source identity, binary identity, report relative
   path/hash, and manifest relative path/hash.
4. Serialize promotions with a retained-root lock and compare the current
   transaction observed at lock acquisition before replacement. The pointer is
   authoritative. Append and fsync `ledger.jsonl` only after pointer replacement;
   `verify`/`recover` reconciles a missing ledger event from `current.json`, drops
   incomplete staging data, and leaves unreferenced completed bundles archived.
   Platform code must use an actual replace primitive (`rename` on POSIX and
   `ReplaceFileW`/`MoveFileExW` on Windows), not sequential report/manifest copies.
5. Add cold and warm scenarios for 1, 2, 32, and 1000 consumers; 100%, 50%,
   and 0% unique keys; vertical, H/V, both-axis, and mixed shapes.
6. Start total timing before any key lookup or plan build and stop after ordered
   result scatter.
7. Preserve separate execution-only CPU and Metal timers for continuity.
8. Add versioned JSON output and record command, build flags, host, sample
   count, median, p95, median absolute deviation, and memory observations.
9. Run cold samples with a fresh cache/service session and warm samples in a
   named retained session. Use at least 21 independent performance samples for
   percentile claims; correctness may run separately from timing.
10. Capture a SHA-256 manifest of relevant sources, CMake inputs, benchmark
   binary, embedded metallib, and report. Do not imply a commit or `HEAD`.
11. Add paired, isolated coefficient pack and upload microbenchmarks for the
   exact S4-S7 payloads. Time the current emitted-per-consumer coefficient
   payload and a benchmark-only unique-plan payload through the same copy and
   staging/upload primitives. Record every raw paired duration, emitted/unique
   byte count, allocation mode, and ordering; do not derive duration by scaling
   bytes.
12. Freeze, per workload, an `addressable_stage_budget` equal to the directly
   timed cold plan-stage median plus the non-negative paired-median emitted-minus-
   unique pack delta and upload delta. This is a target-calibration budget, not
   a claim that the same amount of production wall time is removable. If a
   payload cannot exercise the same primitive or a delta is noise/non-positive,
   set that component to zero and record the limitation. Also retain the direct
   in-pipeline `plan + host_pack + upload` stage total as an upper bound.

Exit gate:

- All benchmark stages sum consistently within documented timer nesting.
- The existing execution-only numbers remain available.
- A repeatable cold 1000-candidate end-to-end baseline exists on the M4 Max.
- No p95 or performance assertion is derived from fewer than 21 samples.
- The baseline report is linked to an exact SHA-256 build/source manifest.
- Every retained promotion survives injected termination at each stage as either
  the complete predecessor or complete successor; `verify` rejects a bad hash,
  and `recover` reconciles pointer-ledger lag without changing the pointer.
- The report contains raw paired pack/upload microbench samples and labels
  `addressable_stage_budget` as calibration rather than measured pipeline wall;
  no byte-proportional time estimate is presented as measured duration.

### M1 - Canonical key, single-flight cache, and bounded residency (P0)

Files:

- `engine/include/getnative/axis_plan.hpp`
- `engine/src/planner/axis_plan.cpp`
- New `engine/include/getnative/scan_error.hpp`
- New `engine/src/analysis/scan_error.cpp`
- New `engine/tests/scan_error_test.cpp`
- New `engine/tests/axis_plan_cache_test.cpp`
- `engine/CMakeLists.txt`

Work:

1. Promote canonical key creation into a tested API and remove the private
   over-specific key.
2. Implement the atomic ready/existing-flight/new-claim disposition and
   move-only claim lifecycle.
3. Publish one success, exception, or pre-submit abandonment to every waiter
   without building or waiting under the cache mutex.
4. Add the public `ScanError` carrier and exact validation, overflow, cancellation,
   stop, resource, abandoned-claim, and unexpected-builder mappings. Store one
   immutable error payload per failed flight.
5. Add generation-and-flight-identity-safe `clear()`, including the old-flight/
   new-same-key ABA case.
6. Add logical byte accounting, oversize no-admit, LRU eviction, monotonic
   cumulative stats, and per-acquisition dispositions for batch accounting.
7. Keep the existing `get_or_build()` call surface as a synchronous wrapper.

Exit gate:

- 32 synchronized equal misses increment `builds_started` by exactly one.
- Warm equal lookup increments no build count.
- Key normalization deduplicates only irrelevant filter fields.
- Waiters do not occupy planner worker slots.
- Equal-flight failures expose byte-equivalent `ScanErrorData`; cancelled waiters
  may independently return `cancelled` without mutating that shared payload.
- Clear followed by a same-key request cannot be erased or populated by an old
  generation's completion.
- Eviction and clear never invalidate externally held plans.
- Frozen plan bytes are unchanged.
- ThreadSanitizer reports no cache race in the focused stress test.

### M2 - Unique-key AxisPlanService and enforced resource permits (P0)

Files:

- New `engine/include/getnative/axis_planner.hpp`
- New `engine/src/planner/axis_planner.cpp`
- New `engine/include/getnative/resource_budget.hpp`
- New `engine/src/scheduler/resource_budget.cpp`
- New `engine/tests/axis_planner_test.cpp`
- `engine/bench/scan_pipeline_benchmark.cpp`
- `engine/CMakeLists.txt`

Work:

1. Implement stable first-occurrence dedup, atomic cache acquisition, and one
   task only for each new build claim. Existing-flight waiters use no worker.
2. Add a reusable fixed worker set, bounded queue, and a shared finite
   `ScanResourceBudget` whose worker policy is calibrated by M0.
3. Refactor only planner temporary allocation into a capped/counting scratch
   resource while preserving arithmetic order. Enforce checked claimed bytes
   and record actual scratch/output logical high water plus external RSS.
4. Add calibrated serial fallback for small/cheap batches.
5. Serialize P0 `build_many()` admission per service while preserving the
   cache's independent single-flight protocol. Before acquisition, enforce
   request-count, pending-claim, retained-plan upper-bound, and overflow gates.
6. Implement the explicit `request_stop()` -> `shutdown()` -> caller joins all
   public calls -> destructor lifecycle, plus cancellation, abandoned claims,
   first-failure, stable-state join, and cache-retention behavior.
7. Report per-batch dispositions, physical builds, admission wait, pending
   claims, actual scratch/output, returned distinct plan bytes, service peaks,
   and worker concurrency.

M2 remediation branch (maximum two experiments total, in this order):

1. Reuse one bounded planner scratch arena per fixed worker.
2. Reserve exact vector capacities from the checked request estimate.

This branch triggers only when the initial M2 run misses the 4x S4 throughput,
S1/S2 latency, or 512 MiB RSS gate. Each experiment is isolated behind a
temporary switch, receives exactly one implementation attempt, and reruns the
same 21-sample M2 command. Keep it only when it improves the failing median by
at least 5% or reduces the failing peak byte/RSS value by at least 10%, preserves
bitwise plan output, passes sanitizers, and does not turn another M2 gate red.
Otherwise remove it before the next experiment. If the mandatory gates remain
red after both experiments, M2 is blocked and returns to planning; M3 cannot
start.

Exit gate:

- Plan vectors match serial request order and pointer sharing.
- All frozen plan bytes match the direct serial builder.
- Current M4 Max 1000-unique cold plan throughput is at least `4x` M0 serial.
- Batch size 1 and 2 median latency regresses by no more than
  `max(5%, 5 us)` versus direct build.
- Planner RSS stays below `512 MiB`; actual logical scratch/output high water
  never exceeds the enforced claims or session budget.
- Worker counts never exceed configured limits.
- Concurrent callers serialize stop-aware; peak pending claims and active
  retained-plan bytes remain within service ceilings, including failure and
  shutdown.

### M3 - Bounded prepared slices and unified CPU budget (P0)

Files:

- New `engine/include/getnative/scan_batch.hpp`
- New `engine/src/scheduler/scan_batch.cpp`
- New `engine/tests/scan_batch_test.cpp`
- `engine/include/getnative/cpu_analysis.hpp`
- `engine/src/backend/cpu/cpu_analysis.cpp`
- `engine/bench/scan_pipeline_benchmark.cpp`
- `engine/CMakeLists.txt`

Work:

1. Introduce candidate plan requests, a bounded adjacent preparation cursor,
   slice-local unique plan table, stable global candidate indexes, prepared
   candidates, and deterministic adapters/scatter.
2. Apply one session resource budget to planner and CPU candidate worker counts.
3. Prepare, execute, and scatter one bounded slice at a time. Keep planning and
   CPU execution phase-separated to prohibit nesting and bound arbitrary scan
   lengths before any overlap work.
4. Add typed cancellation and stage-aware failures without returning partial
   ordered results.
5. Route the benchmark through this path; retain legacy free functions as thin
   compatibility wrappers until CLI integration.

Exit gate:

- H, V, both, repeated ids, repeated plans, and mixed consumers retain input
  result order.
- CPU prepared-slice results are bit-identical to current scalar and batch APIs.
- A horizontal and vertical request with the same normalized key shares one
  plan-table entry.
- Cancellation and injected failure join every worker and leave no live task.
- Full cold/warm counters satisfy documented conservation invariants.
- Every S1-S7 paired `m3_slice` versus `m2_direct` CPU-execution and full-wall
  comparison passes section 6.7. M0-to-M2 planner gain remains separately
  visible and is not subtracted from M3 time.

### M4a - Behavior-preserving GPU batch extraction and Metal adoption (P0)

Files:

- New `engine/include/getnative/gpu_batch.hpp`
- New `engine/src/backend/gpu/gpu_batch.cpp`
- New `engine/tests/gpu_batch_test.cpp`
- `engine/src/backend/metal/metal_backend.mm`
- `engine/include/getnative/metal_analysis.hpp`
- `engine/tests/metal_conformance_test.cpp`
- `engine/bench/metal_benchmark.cpp`
- `engine/CMakeLists.txt`
- `docs/architecture.md`
- `docs/windows-cuda-vulkan-handover.md`

Work:

1. Move POD layouts, checked arithmetic, shape classification, workspace math,
   adjacent tiling, existing per-candidate coefficient flattening, and p1
   partial merge into pure C++ without changing representation.
2. Keep source upload, MTLBuffer allocation, command encoding, submission
   window policy, and pipelines in Metal.
3. Add a test/diagnostic shadow path that runs old and common packers on the
   same inputs and compares every byte, descriptor, tile, workspace value, and
   merged result.
4. Migrate Metal without changing shader code or arithmetic order; retain a
   bounded rollback switch through the mandatory M4b checkpoint.
5. Add pack/upload/execute/readback/merge counters.

Exit gate:

- CPU-only builds compile and test `gpu_batch` with every GPU option disabled.
- POD sizes and every field offset are asserted.
- Packed fixtures cover B3, B7, generic, H, V, both, both forward orders,
  mixed adjacent signatures, workspace limits, and every overflow boundary.
- Handcrafted base packed fixtures produce the reviewed macOS SHA-256 golden.
  If M4c is kept, canonical shared-plan fixtures are added to the reviewed final
  golden; if M4c is removed, the base set remains final. M7's hard Windows lane
  must reproduce the final enabled set before CUDA/Vulkan work may begin; M4a
  does not claim Windows evidence early.
- Shadow comparison is byte-identical for every fixture because M4a has not yet
  changed duplicate representation.
- Existing Metal conformance passes unchanged.
- M0 Metal execution median regresses no more than 5%.
- Metric tolerance, valley distance, workspace `<2 GiB`, and explicit working
  set `<2 GiB` remain green.

### M4b - Mandatory Metal queue and cancellation hardening (P0)

Files: the M4a common/Metal/test/benchmark files.

Work:

1. Preserve M4a's per-candidate coefficient representation byte-for-byte.
2. Add a backend-local maximum queued-ahead policy and a real stop request from
   another thread during Metal execution. Calibrate queue depth so cancellation
   meets the product latency gate without freezing the numeric depth.
3. Reject new submission windows after stop observation, drain already-submitted
   commands to a safe boundary, discard partial output, and leave reusable
   buffers valid for the next call.
4. Remove the private/rollback packer only after byte parity, full conformance,
   the <=5% execution regression gate, and cancellation are green.

Exit gate:

- All packed fixtures, including repeated-plan S6, remain byte-identical to M4a.
- A mid-flight stop submits no window after observation and completes within
  two measured tile durations under the calibrated queue policy.
- M4a correctness, memory, and <=5% execution regression gates remain green.
- M4b is mandatory and atomically becomes the retained Metal predecessor. A
  failure blocks M4c, M5, M6, M7, CUDA M1, and Vulkan M1; performance evidence
  may not revert this safety checkpoint to M4a.

### M4c - Optional unique-plan arena deduplication overlay (P0 evaluation)

Files: the M4a/M4b common/Metal/test/benchmark files.

Work:

1. Against the retained M4b artifact, evaluate one canonical stable-first-
   occurrence layout that packs each unique plan's coefficients once per
   bounded submission window. No second representation variant is permitted.
2. Candidate descriptors reuse coefficient bases while retaining independent
   workspace fields. Shared fixtures compare decoded bases/values and ordered
   results semantically; non-shared fixtures remain byte-identical to M4b.
3. Keep the dedup switch isolated until all correctness, cancellation, memory,
   sanitizer, and performance evidence is complete.
4. If kept, add canonical S5/S6 shared-plan packed fixtures to the final
   cross-compiler SHA-256 golden. If removed, prove the base M4b golden is
   unchanged after the reference rerun.

Keep/remove gate:

- Run 21 alternating same-process M4b/M4c pairs for S4-S7 and define
  `ratio_i = (m4c_i - m4b_i) / m4b_i` for full wall. Any non-positive M4b
  denominator or `MAD(ratio_i) > 0.02` invalidates evidence and reruns the
  unchanged command; noise never widens a threshold.
- `KEPT` requires emitted coefficient bytes to decrease for S5/S6,
  `median(ratio_i) <= 0.05` for every S4-S7 workload, and either
  `median(ratio_i) <= -0.05` for S5 or S6, or at least 20% reduction in S6's
  combined emitted-host-coefficient plus uploaded-coefficient bytes and at
  least 10% S6 total explicit-working-set reduction. All M4b cancellation,
  exactness, tolerance, order, sanitizer, and absolute memory gates stay green.
- Otherwise record `REMOVED_AFTER_TEST`, delete only the dedup representation
  and switch, rerun the unchanged M4b command, and leave/promote the stable
  predecessor to that fresh M4b report. Mandatory cancellation hardening remains.
- A preliminary `KEPT` decision updates the reviewed final-enabled-set golden,
  then reruns the complete M4c command. Only a second valid `KEPT` atomically
  replaces M4b as the retained Metal predecessor; a changed disposition follows
  the removal branch and noisy evidence reruns unchanged.

### M5 - Evidence-triggered bounded GPU pipeline overlap (P1)

Trigger:

- Implement only if M0 plus the current retained M4b/M4c predecessor show
  planner plus pack/upload is at least 10% of full cold wall time. Long scans
  are already memory-bounded by phase-separated M3 slices and are not a reason
  by themselves to add concurrency.

Files if triggered:

- `engine/include/getnative/scan_batch.hpp`
- `engine/src/scheduler/scan_batch.cpp`
- `engine/include/getnative/gpu_batch.hpp`
- `engine/src/backend/gpu/gpu_batch.cpp`
- `engine/src/backend/metal/metal_backend.mm`
- New `engine/tests/scan_pipeline_test.cpp`
- `engine/bench/scan_pipeline_benchmark.cpp`

Work:

1. Add a two-slice bounded state machine: prepare/pack `N+1` while Metal
   executes `N`, reusing the M3 slice semantics.
2. Bound queued candidate count, scratch bytes, host plan bytes, and device
   arena reuse. Do not create nested worker pools.
3. Scatter each completed window by stable input index.
4. Stop producing on cancellation/failure, let an already-submitted device
   window reach a safe boundary, discard partial final output, and join.
5. Retain the phase-separated path as a diagnostic fallback until parity and
   performance are proven.

Exit gate if triggered:

- All results and stage counters match the phase-separated path.
- Cancellation completes within two observed tile durations after the request
  is seen and no later window is submitted.
- Full cold wall time improves at least 10% over the stable retained predecessor;
  otherwise remove the overlap implementation, leave the predecessor artifact
  unchanged, and retain only the streaming-compatible contracts.
- No RSS or explicit GPU working-set gate regresses.

### M6 - Profile-driven optional optimizations (P1/P2)

Planner scratch reuse and exact capacity reservation are no longer M6 items;
they are the bounded M2 remediation branch and cannot be deferred past a failed
M2 gate.

Evaluate the remaining interventions sequentially. Each reads the stable
retained-predecessor artifact produced by the preceding decision, receives at
most two implementation variants, uses 21 samples per reference/variant
command, and must end in exactly one recorded disposition:
`SKIPPED_TRIGGER_FALSE`, `KEPT`, or `REMOVED_AFTER_TEST`. A variant stays behind
an experiment switch until its decision. `KEPT` atomically advances the stable
artifact after every required correctness/memory/sanitizer gate passes. A
non-kept item is deleted, its switch is removed, the unchanged predecessor
command is rerun, and the stable artifact remains byte-identical before the
next item.

#### M6.1 Shared horizontal intermediate reuse

- Trigger only when S7 or S8 reports that at least 25% of candidates have the
  same normalized horizontal key and the separately timed horizontal inverse +
  forward work is at least 10% of backend wall. Identical key/geometry must be
  proven; candidate ids are insufficient.
- Evaluate the direct shared-intermediate design and at most one bounded-lifetime
  variant. Preserve current arithmetic and never share mutable workspace across
  concurrently executing candidates.
- Keep only when the triggered workload's full-wall median improves at least 5%,
  explicit working set increases no more than 5%, and exact CPU/Metal
  conformance, ordering, cancellation, and sanitizers remain green.

#### M6.2 Stable global signature bucketing

- Trigger only when S7 telemetry shows non-adjacent repeated signatures create
  at least 25% more tiles than the computed stable-global-grouping lower bound
  and tile-layout plus submission CPU time is at least 10% of Metal wall.
- Evaluate whole-slice stable grouping and at most one budget-bounded grouping
  variant. Both scatter by original input index.
- Keep only when S7 full-wall median improves at least 5%, result order and all
  metrics remain conformant, cancellation remains within two tile durations,
  and RSS/explicit GPU working set increases no more than 5% while staying under
  absolute limits.

#### M6.3 Cross-frame GPU-resident plan arena

- Trigger only when S8 contains at least three frames, repeated plan-key hit rate
  is at least 80%, and coefficient pack + upload is at least 10% of warm wall.
- Evaluate one bounded LRU arena keyed by plan-format version, GPU device
  identity, and `AxisPlanKey`, plus at most one capacity variant. Capacity and
  entry count are finite configuration; clear/device-loss/version-change paths
  are mandatory.
- Keep only when S8 warm full-wall median improves at least 10%, cold S7 regresses
  no more than 5%, cache accounting stays within its configured limit and the
  2 GiB total, and eviction, cancellation, device-loss simulation, and semantic
  conformance remain green.

If a trigger is false, no prototype is written. If a keep threshold fails, the
implementation is removed even when a microstage improved. All intra-AxisPlan
parallelism remains outside this program, including weight generation, band
construction, LDLT, projection, and solve ordering.

### M7 - Contract freeze and CUDA/Vulkan go/no-go (P0)

Files:

- `docs/architecture.md`
- `docs/windows-cuda-vulkan-handover.md`
- New `docs/performance/pre-gpu-foundation-result.md`
- New `.github/workflows/engine-pre-gpu.yml`
- Public headers introduced above

Work:

1. Freeze versioned AxisPlanKey semantics, plan-format version, GPU semantic-v1
   behavior, POD-v1 sizes/offsets, supported shape bounds, checked workspace
   formulas, stable result ordering, cancellation outcomes, and partial merge.
   Freeze the telemetry schema version with additive-extension rules.
2. Explicitly leave worker auto policy, tile size, submission queue depth,
   transfer/staging strategy, and backend-specific additive data unfrozen.
3. Record benchmark deltas, commands, source/binary SHA-256 manifest,
   host/compiler/build flags, and residual
   risks.
4. Run the Windows CPU-only lane against the proven `gpu_batch` contract rather
   than extracting logic from Metal.
5. Verify optional GPU backends remain default OFF and CPU-only startup has no
   SDK/runtime dependency.
6. Run the named `windows-x64-msvc-pre-gpu` lane on a real Windows Server 2022
   or Windows 11 x64 host with Visual Studio 2022 MSVC. It executes the exact
   test-spec PowerShell block, compares POD fixture hashes with the macOS golden,
   captures PE imports, and uploads the M7 Windows artifact bundle. This is a
   hard CPU-only portability gate, not CUDA/Vulkan runtime evidence.

Exit gate:

- M0-M4b are complete and green, including the mandatory retained M4b
  cancellation checkpoint. M4c has a terminal keep/remove verdict; M5 and every
  M6 item have a terminal keep/skip/remove verdict; and the
  retained-predecessor ledger resolves to the exact final CPU and Metal report
  plus matching SHA-256 manifests.
- All exactness, concurrency, memory, sanitizer, and Metal gates pass. Each
  performance gate either passes or has the specific section 6.7 non-pass
  disposition accepted by a native performance verifier without a success
  claim.
- The `windows-x64-msvc-pre-gpu` CPU-only build/test/startup/import/ABI/hash
  artifact is green. If that real-host evidence is unavailable or red, M7 is
  `BLOCKED_WINDOWS_EVIDENCE`; CUDA M1 and Vulkan M1 must not begin.
- The handover names no host-layout behavior that exists only inside Metal.
- Only then may CUDA M1 or Vulkan M1 implementation begin.

## 8. Overall acceptance criteria

1. Every frozen AxisPlan field/vector is byte-identical before and after.
2. CPU prepared-slice, scalar, and parallel candidate results are bit-identical and
   retain input order across worker counts 1 through the configured maximum.
3. Concurrent equal cache misses start one build; a warm batch starts zero.
4. Cache and planner limits are enforced under success, failure, cancellation,
   clear, eviction, and oversize-plan paths.
5. Cold 1000-unique plan throughput reaches at least `4x` the M0 serial baseline
   on the current 16-core host; small-batch latency stays within its gate.
6. M0 records directly timed in-pipeline stages and raw paired emitted/unique
   pack/upload samples. M3 passes its S1-S7 paired-control gates. Final CPU/S4
   and Metal/S4-S7 results apply section 6.7's frozen budget, MAD noise floor,
   and saving arithmetic individually. A zero/noisy budget produces an explicit
   `NO_MEASURABLE_ADDRESSABLE_BUDGET` verifier disposition, never a zero-target
   performance pass. Byte ratios are never converted into time.
7. Planner peak RSS is `<512 MiB`; explicit GPU workspace and total explicit
   GPU working set are each `<2 GiB`.
8. Metal metric tolerance, argmin distance, top-5 local-valley set (or all
   valleys when fewer), ordering, shape coverage, and cancellation remain
   within existing contracts. Both benchmark and conformance use section 5.6's
   plateau/ranking oracle. Independently selected sets must have equal
   cardinality and their ascending indexes match one-to-one within one step.
9. CPU-only configure/build/test/startup requires no Metal, CUDA, or Vulkan SDK
   or runtime on both macOS and the real `windows-x64-msvc-pre-gpu` lane. The
   Windows POD/fixture SHA-256 output equals the macOS golden and PE imports
   contain neither `nvcuda.dll` nor `vulkan-1.dll`.
10. CUDA/Vulkan start from the common tested POD, packing, tiling, merge, and
    prepared-scan contracts rather than copying Objective-C++ helpers.

## 9. Pre-mortem

### Failure 1: planner throughput rises but full scans slow down

Cause: excessive workers, allocator contention, cache-line pressure, or plan
production outruns the GPU and increases RSS.

Prevention: full-wall M0 baseline, serial threshold, fixed workers, scratch
permits, separate CPU/GPU stage timers, and a keep/remove gate for overlap.

### Failure 2: common packing silently changes Metal results

Cause: descriptor offset drift, changed plan order, duplicate-plan base reuse,
or a different Float32 partial merge order.

Prevention: compile-time layout assertions, CPU-only golden packed fixtures,
exact descriptor/base tests, fixed merge tests, full Metal conformance, and no
shader arithmetic change in the migration milestone.

### Failure 3: cancellation or cache clearing deadlocks the pipeline

Cause: a waiter owns the cache lock, a producer blocks on a full queue after the
consumer fails, or an old in-flight build republishes into a cleared cache.

Prevention: separate ready/in-flight state, generation-safe clear, no build or
wait under cache lock, stop-aware bounded waits, strict join ownership, hostile
failure/cancellation tests, and ThreadSanitizer.

### Failure 4: interrupted promotion publishes mixed or unverifiable evidence

Cause: the process or host stops after one evidence file changes, after the
authoritative pointer changes, or before the append-only ledger records the new
transaction.

Prevention: immutable per-transaction bundles, file/directory fsync, verified
hashes, one atomic same-directory `current.json` replacement, an old-or-new
recovery invariant, restart failpoints around every durable step, and explicit
`current_transaction`, `recovered_transaction`, `ledger_reconciled`, and orphan
bundle telemetry. A ledger entry may lag and be reconstructed; the pointer never
depends on the ledger to identify the retained implementation.

## 10. Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| Key canonicalization merges distinct plans | Canonicalize only irrelevant fields; exact relevant bits; byte matrix before enabling each rule |
| Old cache completion corrupts a new generation | Generation plus unique flight identity; identity-checked publish/remove; explicit ABA stress test |
| Concurrent batch calls create unbounded claims/retained outputs | Serialize P0 service admission; preflight request/claim/output ceilings; count active and caller-retained distinct bytes |
| Service destruction races an active member call | Explicit stop/shutdown while alive; owner joins all public calls before destructor; TSan lifecycle stress |
| Logical byte accounting understates allocator RSS | Report logical bytes and process RSS separately; enforce RSS benchmark gate |
| LRU bookkeeping makes hot hits expensive | O(1) list/map updates under a short lock; benchmark warm 1/32/1000 paths |
| Worker pool adds small-batch overhead | Calibrated serial path and explicit 1/2-plan regression gate |
| Interrupted evidence promotion tears report/manifest identity | Immutable bundle plus atomic current pointer; fsync and crash-recovery failpoints; ledger reconciliation after pointer commit |
| Internal exceptions drift into backend-specific public behavior | One `ScanError` carrier, frozen code/stage/backend/retry mapping, equal-flight payload tests, and boundary mapping tests |
| Shared plan packing masks extraction or cancellation regressions | Split M4a byte-identical extraction, mandatory M4b cancellation hardening, and removable M4c semantic dedup; retain exact predecessor artifacts |
| Global signature grouping changes observable order | Keep adjacent grouping in P0; require stable scatter and profile before rebucketing |
| Pipelining obscures attribution | Add only after M4c disposition, retain phase-separated diagnostic path, remove if <10% win |
| One platform-specific optimization leaks into common code | Common code owns POD/data math only; device allocation, transfer, queues, and pipelines remain backend-local |

## 11. ADR

### Decision

Adopt the layered measured foundation in Option B. Build and prove the plan
service and bounded prepared-slice contract first, then extract and adopt a pure
C++ GPU batch contract in Metal, then retain a mandatory cancellation-hardened
checkpoint before evaluating shared-plan deduplication. Add pipeline overlap
and deeper optimizations only at explicit profile gates. Keep every stage
inside each AxisPlan serial.

### Drivers

- Stable reusable host contracts are required before two more GPU backends.
- Exact numerical behavior and deterministic ordering are product contracts.
- Full scan wall time, bounded memory, and diagnosability matter more than an
  isolated microbenchmark.

### Alternatives considered

- Planner-only patch: rejected as the final stopping point because it leaves
  CUDA/Vulkan host duplication and later ABI churn.
- Full async backend framework: rejected because device/job evidence is not yet
  sufficient to justify its policy surface.
- Intra-plan LDLT parallelism: rejected because dependencies, narrow bandwidth,
  synchronization, and strict Float64 ordering make it a poor first target.

### Why chosen

The selected approach creates measurable value at every milestone while
placing only already-shared mathematical/layout behavior in common code. It
lets Metal serve as the real-device proof before CUDA/Vulkan depend on the
contract.

### Consequences

- CUDA/Vulkan implementation begins later, but with less duplicated host work
  and lower contract churn.
- The plan/cache/bounded-preparation API becomes a maintained engine surface;
  full job scheduling remains deferred until production `analyze` exists.
- P0 serializes batch-call admission within one service to keep pending claims,
  returned-plan construction, shutdown, and failure bounded. Independent plan
  keys still build concurrently inside the admitted batch, and cache
  single-flight remains concurrency-safe for future policy.
- Metal undergoes a byte-preserving extraction, a mandatory independently
  evidenced cancellation-hardening checkpoint, and only then a removable
  representation optimization before other GPU work.
- A noisy/zero addressable budget is reported as no measurable benefit rather
  than converted into an automatic performance pass.
- Retained evidence is addressed by one crash-consistent pointer to an immutable
  bundle; the ledger is audit history and can be reconciled from that pointer.
- Public scan failure semantics are versioned engine data, not inferred from
  standard-exception text or backend-native error messages.
- Real Windows CPU-only ABI/hash/import evidence is a hard backend-start gate;
  lack of a Windows lane delays CUDA/Vulkan rather than weakening the claim.
- Some theoretically attractive optimizations remain deliberately deferred.

### Follow-ups

- Implement M0-M4b in order, obtain the explicit M4c keep/removal decision, then
  terminal decisions for M5 and every M6 item.
- Run the real `windows-x64-msvc-pre-gpu` lane and freeze/update the Windows
  CUDA/Vulkan handover at M7.
- Start CUDA generic vertical strict MVP first; start Vulkan only after the
  common contract has also survived CUDA host integration or an explicit
  parallel-lane ownership plan prevents conflict.

## 12. Available agent types and follow-up staffing

Available relevant native roles include `architect`, `executor`,
`test-engineer`, `verifier`, `code-reviewer`, `debugger`, `git-master`, and
`vision`; the wider catalog also includes `planner`, `critic`, `researcher`,
`dependency-expert`, `writer`, and `code-simplifier`.

Recommended implementation staffing:

- Leader/Ultragoal owner: one high-reasoning coordinator owns milestone order,
  resource/contract decisions, integration, and durable evidence.
- Planner/cache lane: one medium-reasoning `executor` owns axis-plan headers,
  cache, planner service, and focused tests for M1-M2.
- Slice/GPU batch lane: one medium-reasoning `executor` owns bounded scan
  preparation and pure C++ packing after M2 contracts land.
- Metal adoption lane: one medium-reasoning `executor` owns Objective-C++
  migration and real-device tests after `gpu_batch` is green.
- Test/performance lane: one medium-reasoning `test-engineer` owns benchmark
  fixtures, hostile concurrency/cancellation cases, and evidence tables; it
  must not approve its own implementation lane.
- Verification lane: one high-reasoning native `verifier` independently checks
  exactness, counters, sanitizers, performance claims, and CUDA/Vulkan readiness.

Do not run write lanes concurrently when they share public headers or CMake.
M1 must land before M2, M2 before M3, M3 before common `gpu_batch`, M4a before
mandatory M4b, and M4b before optional M4c. Benchmark/test work may proceed in
parallel only on non-overlapping files with an agreed schema.

## 13. Execution and goal-mode follow-up suggestions

This is a performance optimization program, so the recommended durable follow-up
is `$performance-goal`, using this PRD, the test spec, M0 benchmark JSON, and the
acceptance gates as evaluator context. `$ultragoal` remains the default general
ledger when one durable sequence is preferred.

For parallelizable milestones, use `$ultragoal` plus `$team`: Ultragoal owns the
milestone ledger; Team returns checkpoint-ready diffs, tests, benchmark JSON,
and residual-risk notes. Example launch hints after explicit approval:

```text
$performance-goal .omx/plans/prd-pre-gpu-scan-foundation.md
$team 3:executor "Implement the approved current milestone only; preserve ownership boundaries and return test/benchmark evidence"
omx team 3:executor "Implement the approved current milestone from .omx/plans/prd-pre-gpu-scan-foundation.md"
```

`$ralph` is only an explicit fallback for a deliberately single-owner,
persistent fix/verify loop; it is not the recommended durable ledger for this
multi-milestone program.

### Team verification path

Before a Team milestone shuts down, it must provide: owned-path diff inventory,
focused tests, warnings-as-errors build, sanitizer status where applicable,
benchmark JSON with baseline comparison, memory counters, and known gaps. A
native `verifier` then checks those artifacts against the test spec. Ultragoal
or Performance Goal records completion only after that independent evidence is
green; otherwise it opens a bounded remediation milestone.

## 14. Planner draft changelog

- Replaced candidate/axis task identity with unique canonical plan-key identity.
- Separated existing CPU/Metal parallel behavior from genuinely missing work.
- Added concrete API targets, lifecycle semantics, file ownership, and ordered
  milestones.
- Made pipeline overlap and micro-optimizations conditional on stage evidence.
- Added full-wall, exactness, single-flight, memory, sanitizer, Metal, and
  pre-CUDA/Vulkan freeze gates.
- Applied Architect review 1: added atomic cache claims and ABA-safe generation
  rules; replaced all-batch preparation with bounded slices; made actual logical
  allocation enforceable; split M4 extraction from dedup; added shadow,
  mid-flight cancellation, top-k valley, SHA-256, and versioned-freeze gates;
  removed every intra-AxisPlan parallelism option.
- Applied Architect review 2: separated stop/shutdown from destruction;
  serialized P0 service-call admission; bounded pending claims and active/
  returned plan bytes; defined the exact plateau/ranking/top-k matching oracle;
  replaced unmeasurable `avoidable_wall` language with directly timed stages
  and paired pack/upload calibration samples.
- Applied Critic review 1: added exact CMake/CTest/sanitizer/benchmark/Metal
  validation and Windows commands; replaced M3 attribution with a paired M2
  control; added MAD-aware final full-wall arithmetic and explicit no-benefit
  dispositions; moved allocator remedies into bounded M2 remediation; gave
  every M6 item a trigger, experiment cap, keep/delete threshold, and skip rule;
  made real Windows CPU-only ABI/hash/import evidence a hard M7 prerequisite;
  added a stable retained-predecessor artifact so every conditional experiment
  compares with and advances from the implementation actually kept.
- Applied Architect review 4: split mandatory queue/cancellation hardening into
  an independently retained M4b checkpoint and moved removable unique-plan
  dedup to M4c, so every artifact names the exact executable it proves.
