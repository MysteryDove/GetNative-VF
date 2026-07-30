# Test Specification: Staged Pre-GPU Foundation, Stage 0/1

## 1. Purpose

Prove only two things:

1. the existing benchmarks expose planner and total time without changing the
   existing execution results;
2. bounded parallel construction of unique AxisPlans is exact and improves the
   measured full benchmark enough to justify keeping it.

This specification does not gate later scheduler, packing, CUDA, or Vulkan work.

## 2. Frozen Baseline

Record the current executable outputs before source changes:

```text
core: banded_us=80.992, batch32_ms=9.335, batch_candidates_per_s=3427.944
metal full: cpu_ms=1538.473, metal_ms=111.097, speedup=13.848x
metal correctness: maximum_metric_error=5.61e-8, valley_step_distance=0
metal memory: workspace=168.750 MiB, working_set=252.230 MiB
```

These values are a local snapshot, not universal thresholds. The comparison run
must use the same host, build type, flags, fixture, and benchmark arguments.

## 3. Stage 0 Benchmark Checks

Extend the existing benchmarks rather than create a new framework.

- `plan_ms` starts immediately before the candidate plan/cache loop and stops
  after all candidate plan references are ready.
- Existing `cpu_ms` and `metal_ms` keep their current execution-only boundaries.
- `cpu_total_ms` and `metal_total_ms` include `plan_ms` plus the matching
  execution stage; no overlap is claimed.
- `--samples N` performs one warmup and N measured runs.
- `--json-out PATH` writes arguments, host/compiler/build identifiers, each raw
  sample, median, and the existing correctness/memory fields.
- A timestamped output directory is immutable after the run. No mutable retained
  pointer or promotion mechanism exists in Stage 0/1.

Stage 0 passes when the new execution-only medians are within 5% of the current
benchmark medians and all existing `--assert` checks remain green.

## 4. Stage 1 Functional Tests

Add `engine/tests/axis_planner_test.cpp`.

### Keying and deduplication

- Empty input returns empty output.
- One request builds one plan.
- 1000 identical requests build once and return 1000 equal pointers.
- Alternating A/B requests build twice and preserve request order.
- Invalid requests fail before worker launch.
- Cache-ready requests do not increment the physical-build count.

### Exactness and ordering

- Compare every `AxisPlan` scalar/vector byte-for-byte against serial
  `build_axis_plan()` for bilinear, bicubic, spline36, spline64, Lanczos3,
  Lanczos8, representative sizes, shifts, and borders already covered by tests.
- Repeat with worker counts 1, 2, 4, 8, and auto.
- Build completion order must not change output order or pointer sharing.

### Bounded concurrency and failure

- A test hook records active builds; peak activity never exceeds the worker cap.
- One injected build failure joins all workers before rethrow and starts no later
  task after the failure is observed.
- Run the focused planner test under ThreadSanitizer.
- Stage 1 does not define a new public error ABI; existing exception behavior is
  preserved at existing public call sites.

## 5. Performance Protocol

Cases:

| Requests | Unique keys | Purpose |
| ---: | ---: | --- |
| 1 | 1 | launch overhead |
| 2 | 2 | small-batch overhead |
| 32 | 32 | moderate scaling |
| 1000 | 1000 | primary independent-plan case |
| 1000 | 1 | dedup/cache case |

Use worker counts 1, 2, 4, 8, and auto. Use a fresh cache for cold samples and a
named reused cache for warm samples. Iteration runs use 7 samples; the adoption
decision uses 21. Report raw samples and median; reject a run if unrelated host
load or build identity changes.

Required gates:

- exactness and all existing conformance checks: pass;
- 1000-unique cold plan median: at least 2x faster than Stage 0 serial;
- 1/2 request median: no more than `max(10%, 10 us)` slower than serial;
- Metal execution-only median: no more than 5% regression;
- full `metal_total_ms`: at least 5% improvement for automatic adoption.

## 6. Verification Commands

Current baseline, already run successfully:

```sh
build/engine-perf/getnative_core_benchmark --assert
build/engine-metal/getnative_metal_benchmark --full --assert
```

Stage 0/1 release verification after implementation:

```sh
cmake -S engine -B build/stage1-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DGETNATIVE_ENABLE_METAL=ON \
  -DGETNATIVE_BUILD_UPSTREAM_CONFORMANCE=ON
cmake --build build/stage1-release --parallel
ctest --test-dir build/stage1-release --output-on-failure
build/stage1-release/getnative_core_benchmark \
  --planner-batch --samples 21 \
  --json-out artifacts/stage1/core-final.json --assert
build/stage1-release/getnative_metal_benchmark \
  --full --samples 21 \
  --json-out artifacts/stage1/metal-final.json --assert
```

Focused ThreadSanitizer verification:

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

If the local compiler/runtime cannot execute TSan, Stage 1 is not called fully
verified until an equivalent supported host runs the same focused test.

## 7. Decision Output

End Stage 1 with one short table:

| Metric | Baseline | Stage 1 | Delta | Gate |
| --- | ---: | ---: | ---: | --- |
| 1000-unique plan median | | | | |
| 1-request plan median | | | | |
| Metal execution median | | | | |
| Metal total median | | | | |
| Correctness/conformance | | | | |

Then record exactly one recommendation:

- `ADOPT_AND_STOP`: Stage 1 passes; keep it and write the next stage plan only if
  a measured stage justifies one.
- `REVISE_ONCE`: correctness is green and the result is close enough that one
  named bottleneck has a bounded fix.
- `REVERT_AND_REDIRECT`: correctness/regression fails or total impact is too
  small; remove the batch path and choose another measured direction.

No Stage 2 implementation starts from this document.
