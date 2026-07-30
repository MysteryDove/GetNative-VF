# Pre-GPU Planner Performance Results

## Stage 0: Planner Measurement

- Branch: `perf/pre-gpu-stage1`
- Planning consensus commit: `d8639be`
- Evaluator: `scripts/run_planner_evaluator.sh`
- Run directory: `artifacts/stage0/20260730T185414Z`
- Engine source identity:
  `3dc3de3d506e0ad78f986e62183c01e5b4c87c19b58620a953af05bcfa0167de`
- Core JSON SHA-256:
  `c31f3eb54a49624bc9f09421de51704562a91591016147597a13336b08f389e5`
- Metal JSON SHA-256:
  `f9b0f969a3bef269c9e0862a92fd64709a657d01b14b51219fc9f748006849fe`
- Host-load snapshot SHA-256 (before/after):
  `e7d18443626c5205aa0055782b17f417d4e73c81213edd2dbac5597fe8589188` /
  `cb1d4b31662e0aeb79c0132744d33a342a36430c025417f7fc7b550f8ecce8a0`
- Process snapshot SHA-256 (before/after):
  `ed82aa9632cf6c991cba47e133faa8e12997c8baa0a809faf9fda0a7fe839b05` /
  `f8a3bbbc19db829f5cc091ead8b8fc90826ffee5237dfdc486a185823c2a71ca`

The immutable JSON files are gitignored runtime evidence. Their hashes and
decision facts are retained here for the later integration handoff.

### Verification

- Release build with Metal and upstream conformance: pass.
- Release Metal/upstream `ctest`: 8/8 pass.
- CPU-only/upstream `ctest`: 7/7 pass.
- Core 21-sample `--assert`: pass.
- Metal full 21-sample `--assert`: pass.
- Raw JSON sample counts, statistics, formulas, and schema checks: pass.
- Atomic JSON collision checks: pass for both a preexisting final path and a
  final path created after validation but before publication. Neither path is
  overwritten and no evaluator `.tmp` file remains.
- Embedded build metadata includes C++23, common and per-configuration flags,
  and the benchmark targets' warning and strict-FP options.

### Results

| Metric | Median | MAD |
| --- | ---: | ---: |
| Core 2-unique planner | `0.111750 ms` | `0.005416 ms` |
| Core batch32 CPU execution | `10.412500 ms` | `0.288625 ms` |
| Metal 1000-unique planner | `107.867666 ms` | `0.761500 ms` |
| Metal execution only | `111.001584 ms` | `5.233543 ms` |
| Metal phase-separated total | `219.284584 ms` | `5.698377 ms` |
| Planner share of Metal total | `0.497607` | `0.009849` |

Correctness and resource results:

- maximum metric error: `5.6093972883308751e-8`;
- valley step distance: `0`;
- Metal peak workspace: `176947200` bytes (`168.750 MiB`);
- Metal explicit working set: `264482784` bytes (`252.230 MiB`).

### Noise assessment

The evaluator refuses to start the build, core benchmark, or Metal benchmark
while any other `getnative_*benchmark` process is active. It also retained
argument-free `comm` process snapshots and `uptime` output before and after the
run. No competing GetNative benchmark appears in either snapshot.

The host remained busy with unrelated Chrome, WindowServer, Spotlight, and
other desktop processes; load averages moved from `10.46 18.88 20.91` before
the run to `31.93 23.19 22.37` after it. Absolute cross-run timing comparisons
are therefore not justified from this result. Within the decision population,
however, planner time stayed in `106.236292..112.886542 ms` with
`0.761500 ms` MAD. Planner-share samples ranged from `0.418383` to `0.516278`
with `0.009849` MAD, and every sample remained more than four times the `0.10`
proceed threshold. The Stage 0 direction is not sensitive to the observed host
load.

### Decision

`PROCEED_STAGE1`

The required statistic is:

```text
median(plan_ms_i / metal_total_ms_i) = 0.49760744618056257 >= 0.10
```

Stage 1 private, cache-independent unique-plan batch construction is therefore
authorized. No later planner/cache/service stage is authorized by Stage 0;
each remains conditional on the next measured result.

The earlier `20260730T173954Z` run is retained only as diagnostic history. Its
source identity predates the final Stage 0 fixes and it is not gate evidence.

## Stage 1: Bounded Unique-Plan Batch Construction

- Branch: `perf/pre-gpu-stage1`
- Evaluator: `scripts/run_planner_evaluator.sh stage1`
- Run directory: `artifacts/stage1/20260730T193106Z`
- Engine source identity:
  `4e647ebd58c7f524f331123deb15d556c3f05fe2bf45367f3fc56876a8ee33c4`
- Core executable FNV-1a 64: `ec515f53ee2cd181`
- Metal executable FNV-1a 64: `5db1d4b78d3ae740`
- Core JSON SHA-256:
  `c0efb6341ec3ec7bbb071a813a7602825c9e67a55300edc135edc60e0d5fedc4`
- Metal JSON SHA-256:
  `0a3a95a7802a244b1ce168a2363c9f249b66507b74595869edbb8902ef13f8df`
- Host-load snapshot SHA-256 (before/after):
  `dd55b1415f4b490702b10e0696ba491d3fb76f3bba6d9f67812ce7b9b0dbe7e1` /
  `ebdf63121a1ce20d14ef333b7805ee74702188d89806ff75d23b384acf63bd1a`
- Process snapshot SHA-256 (before/after):
  `716f80393d9660ea0ab40e18ce8cd7e656495c49808d40054d97571bac95583b` /
  `4d7b40bf1ddf2e09b7ebfbf6ecaff3ecdac1db9bfbacbab28809a05a72143f0b`

The immutable JSON and host snapshots are gitignored runtime evidence. Their
hashes and the decision facts below are retained for integration.

### Verification

- Release build with Metal and upstream conformance: pass, `9/9` tests.
- CPU-only build with upstream conformance: pass, `8/8` tests.
- Focused `getnative_axis_planner_tests` under ThreadSanitizer: pass, `1/1`.
- Core and Metal decision artifacts each contain 21 alternating-order pairs and
  report `stage1_measurement_status=MEASURED`.
- Current source identity, compiled definitions, JSON identities, and both
  executable hashes agree.
- All raw sample counts, pair orders, deltas, speedups, overheads,
  medians, MADs, minima, maxima, and phase-total formulas were independently
  recomputed from the JSON and agree with the stored values.
- Stable exact-key deduplication, byte-exact plan output, requested output
  order, worker caps, deterministic failure selection, stop-claiming behavior,
  and join-before-rethrow are covered by the focused planner test.
- Existing `AxisPlanCache` tests remain green. The batch helper has no
  cross-call state and is consumed only by benchmark pre-execution paths.
- Both process snapshots contain no competing GetNative benchmark, and the run
  directory contains all six expected files with no `.tmp` residue.

### Results

| Metric | Serial | Batch | Paired delta/speedup | Gate |
| --- | ---: | ---: | ---: | --- |
| Core 1000-unique auto plan median | `105.617584 ms` | `15.807875 ms` | `6.653403x` | `>=2x`: pass |
| Metal 1000-unique plan median | `105.683750 ms` | `15.832875 ms` | `6.661206x` | `>=2x`: pass |
| Metal plan paired-delta MAD | - | - | `0.003829` | `<=0.025`: pass |
| 1-request auto plan median | `143.958 us` | `143.875 us` | `0.500 us` paired overhead | `<=14.396 us`: pass |
| 2-request auto plan median | `204.167 us` | `209.334 us` | `4.458 us` paired overhead | `<=20.417 us`: pass |
| Metal execution median | `102.596042 ms` | `102.489833 ms` | `+0.0529%` paired regression | `<=5%`: pass |
| Metal total median | `209.773499 ms` | `118.322708 ms` | `43.5702%` paired improvement | `>=5%`: pass |
| Metal-total paired-delta MAD | - | - | `0.007216` | `<=0.025`: pass |
| Correctness/conformance/TSan | pass | pass | exact/tolerance gates green | pass |

Every explicit and auto 1/2-request case passes its overhead bound. Across all
ten small cases, the largest paired median overhead is `5.667 us`, below its
`22.058 us` bound. Core auto mode used eight workers for 1000 unique keys and
reported peak concurrency eight. The Metal batch path likewise built all 1000
unique keys once with peak/effective concurrency eight.

Correctness and resource results:

- maximum metric error: `5.6093972883308751e-8`;
- valley step distance: `0`;
- Metal peak workspace: `176947200` bytes (`168.750 MiB`);
- Metal explicit working set: `264482784` bytes (`252.230 MiB`).

### Noise Assessment

Host load averages rose from `8.29 6.91 8.59` before the paired run to
`46.98 19.82 13.34` after it, so this result does not support absolute timing
comparisons with a different run. The decision uses same-process alternating
pairs. Metal plan paired-delta MAD is `0.003829` and Metal-total paired-delta
MAD is `0.007216`, both well below the `0.025` validity limit. The performance
gates are therefore not sensitive to the observed host-load change.

### Decision

`ADOPT_AND_STOP`

All correctness, conformance, TSan, variance, small-batch overhead, planner
speedup, Metal execution-regression, and Metal-total improvement gates pass.
Keep the private bounded batch planner and stop Stage 1 without a revision.

After batching, planner share of the phase-separated Metal total has median
`0.132946`, still above the PRD's `0.10` direction threshold. This selects one
small follow-up measurement stage for persistent worker reuse. It does not
authorize cross-call single-flight, persistent plan caching, a broader
scheduler, or GPU-backend changes; those remain skipped unless later benchmark
evidence independently justifies them.
