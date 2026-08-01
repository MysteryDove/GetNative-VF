# CPU memory-profile benchmark (v4)

Binary: `getnative_cpu_memory_profile_benchmark.exe`

Source: `engine/bench/cpu_memory_profile_benchmark.cpp`

Artifact: `<artifact-root>/cpu-memory-profile-results.json`

Artifact publication is atomic and no-overwrite. Use a fresh artifact root for
each run; an existing result is rejected before fixture preparation begins.

This benchmark has two independent workloads:

- Frontend: generate a candidate grid, construct plans for one of six filters,
  and optionally execute the typical 810-height search.
- Backend: prepare one locked 810-height plan and analyze a logical frame
  stream using a bounded structured or decorrelated source ring.

The filter ids are `bilinear`, `bicubic-catrom`, `spline36`, `lanczos3`,
`lanczos5`, and `lanczos8`.

## v4 measurement contract

- `PlanBatchStats` never owns plan handles. Execution uses transient handles
  which are released before `memory_teardown`.
- Frontend `memory_baseline` is before request generation. `memory_teardown` is
  after request containers, cache, plans, and execution temporaries are gone.
- Verification `before_ring` is before source allocation. `after_ring_teardown`
  is captured after both optional rings have been destroyed.
- A single `PersistentFrameExecutor` keeps the same worker threads and one
  workspace per worker across the 1/10/100/1000 checkpoint barriers.
- Parallel workers write results by logical frame index. Ordered checksums and
  fingerprints are reduced only after all workers finish.
- `source_physical_total_bytes` is the sum of the physical source rings actually
  created by that process. It is not a logical-frame byte count.
- Windows artifacts include detected LLC bytes, the configured ring/LLC ratio,
  and whether the ring reaches the 2x LLC pressure target. Add
  `--require-cache-pressure` to make failure to reach that target fatal.
- `OutputDebugStringW("GETNATIVE_PHASE ...")` and stdout `PHASE` lines are debug
  markers only. They are not an AMD uProf range API.

The default backend shape has 1000 logical frames and 32 physical source
frames. Each physical frame is generated independently; logical frames cycle
through the ring using a coprime step. This is a bounded cache-pressure stream,
not a claim that 1000 distinct full-resolution frames remain resident.

## Frontend arms

`height-search-typical` uses destination height 810 and N fractional active
lengths generated through the public candidate-grid API.

`height-envelope` uses N distinct integer destination heights across the
configured inclusive range. It is planner-only and rejects `exec` and `e2e`.

The arms are:

- `cold`: request generation is setup; the measured operation is one cold plan
  batch.
- `warm`: a cold batch is setup; the measured operation is the warm cache-hit
  batch.
- `mixed`: interleave requests across the selected filters, then run cold and
  warm batches. JSON sets `is_cache_pressure_benchmark=true` only when cache
  admission limits actually caused misses or rebuilds.
- `exec`: request generation and cold planning are setup; the measured operation
  executes the retained plan set once.
- `e2e`: one contiguous request-generation, cold-plan, and execution operation.
- `all`: diagnostic full coverage. Do not use it for a single profiler capture.

The frontend correctness gate checks exact plan/result counts and scalar
references for the first and last executed candidates. A structured input can
legitimately produce zero-valued errors, so this sampled comparison is not a
full numeric oracle.

## Profiler capture

`--profile-mode` applies defaults only to omitted arguments. With no explicit
suite/arm/cohort it selects `verification`, `exec`, and `decorrelated`.
Explicit values, including `--cohort both`, are preserved.

A direct profiler launch attributes the whole process. For example, a warm run
also contains request generation and its cold setup before the warm phase. Use
the `PHASE` markers to interpret that capture.

```powershell
$exe = "build/engine-win-x86-agent/getnative_cpu_memory_profile_benchmark.exe"

# One cold planner arm.
& $exe --artifact-root $out --suite height-search-typical `
  --filter bicubic-catrom --arm cold --profile-mode --assert

# One backend bandwidth arm with a decorrelated source ring.
& $exe --artifact-root $out --suite verification --filter lanczos8 `
  --cohort decorrelated --arm exec --profile-mode `
  --require-cache-pressure --assert
```

For strict measured-region attribution, start the program with `--attach-gate`.
It performs setup, prints `ATTACH_GATE`, and waits immediately before the
selected operation. Attach the profiler, configure collection, then press
Enter. Attach-gate runs require one filter, one suite, one non-`all` arm, and
one verification cohort.

```powershell
& $exe --artifact-root $out --suite height-search-typical `
  --filter spline36 --arm warm --profile-mode --attach-gate --assert
```

Point uProf at the executable, not a wrapper batch file. Typical collection
profiles are `assess`, `data_access`, and `ibs`.

## Memory checkpoints

Verification memory mode (`--timing-samples 1`) records:

- process memory before and after ring preparation;
- memory before and after each filter plan;
- exact completed-frame checkpoints at 1, 10, 100, and 1000 when present;
- the final logical-frame checkpoint;
- per-filter teardown and final ring teardown.

Windows `--assert` also requires every applicable memory snapshot to have been
captured successfully. Peak process working set remains a process-lifetime high
water mark. A teardown checkpoint is an ownership boundary, but the Windows C++
heap may retain committed pages, so current private bytes are not required to
return exactly to baseline.

## Timing mode

Timing is separate from profiler and checkpoint runs. With
`--timing-samples N`, where N is greater than 1, the benchmark runs one discarded
warmup followed by N retained samples. Each sample executes the complete logical
stream continuously, without the memory-checkpoint barriers.

```powershell
& $exe --artifact-root $out --suite verification `
  --filter bicubic-catrom --cohort decorrelated --arm exec `
  --timing-samples 21 --assert
```

JSON retains the raw timing array plus median, MAD, minimum, and maximum. It also
retains warmup/sample checksums and fingerprints. `formal_timing_eligible` is
true only for at least 21 retained samples when warmup correctness, every sample,
and all deterministic consistency checks pass.

## Defaults and cautions

- `--candidates 500`
- `--logical-frames 1000`
- `--ring-frames 32`, approximately 253 MiB per 1920x1080 float ring
- `--frame-workers 1`
- `--timing-samples 1`
- `--arm all --cohort both` for a diagnostic full run

The default `all/both` run can hold two source rings, approximately 506 MiB at
1920x1080, in addition to plans and workspaces. Use one suite/filter/arm for
profiling and reserve the full run for functional coverage.
