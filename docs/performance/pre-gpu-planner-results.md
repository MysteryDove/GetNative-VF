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
