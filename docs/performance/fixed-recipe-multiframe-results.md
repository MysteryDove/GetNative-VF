# Fixed-Recipe Multi-Frame Benchmark

## Scope

This benchmark measures the verification workload that the candidate-sweep
benchmarks do not cover: one immutable candidate and one vertical `AxisPlan`
reused across many predecoded frames.

- Host: Apple M4 Max, 12 performance cores, 4 efficiency cores, 40 GPU cores.
- Fixture: `1920x1080 -> 810`, vertical axis, p=1 metric, one locked candidate.
- Frames: `1`, `2`, `10`, `100`, and `1000` logical frames.
- Input storage: deterministic ring of 8 predecoded single-plane float frames.
- Filters: Bilinear, Bicubic Catrom/Mitchell, Spline16/36/64, and Lanczos3-6.
- CPU baseline: current public `analyze_batch_f32`, one candidate and one call per frame.
- CPU experiment: bounded 16-worker frame cursor with one reusable workspace per worker.
- Metal baseline: current serialized `MetalAnalysisEngine` call per frame.
- Samples: 3 rotating-order samples, with 5-sample replacements for Lanczos3/5.
- Run directory: `build/fixed-recipe-results/20260731T065456Z`.
- Selected aggregate: `selected-summary-v3.json`.
- Selected artifact checksums: `selected-sha256-v3.txt`.
- Source identity: `c73dc4d02279cec4b638af1c7dad621244c20dc0a5416bc2fa4debaf3fb77f77`.
- Executable FNV-1a 64: `dac05b650d91aa01`.

Lanczos1/2/7/8 are excluded from this decision set. Their earlier raw files are
retained but are not referenced by the V3 aggregate or checksum manifest.

The dedicated executable is `getnative_fixed_recipe_benchmark`. It reports raw
samples plus median/MAD for total wall, throughput, CPU variants, Metal runtime
phases, correctness, and result stability. Decode and color conversion are
reported as unavailable, not zero. Metal readback, CPU merge, and plan packing
remain explicitly unavailable as independent timers and are included in the
host residual.

## Scaling Across Frame Counts

The table reports the median across the 10 retained filters. `CPU frame / Metal`
is above `1.0x` when the frame-parallel CPU prototype is faster. Ratios are
computed within each paired sample before cross-filter aggregation.

| Frames | CPU frame speedup | Metal speedup vs current CPU | CPU frame / Metal | CPU-leading filters |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 1.00x | 1.40x | 0.72x | 0/10 |
| 2 | 1.97x | 1.68x | 1.18x | 8/10 |
| 10 | 8.46x | 2.01x | 3.91x | 10/10 |
| 100 | 10.94x | 1.82x | 5.78x | 10/10 |
| 1000 | 11.01x | 1.72x | 5.74x | 10/10 |

Metal remains the better single-frame path, but fixed-Recipe verification
crosses over at two frames for most filters. From 10 frames onward every
retained filter favors the frame-parallel CPU prototype.

## 1000-Frame Results

`CPU frame / Metal` is how many times faster the frame-parallel CPU prototype is
than the current serialized Metal path. Metal speedup is relative to the current
one-candidate CPU public API.

| Filter | CPU current ms | CPU frame ms | CPU speedup | Metal ms | Metal speedup | CPU frame / Metal |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Bilinear | 3754.6 | 320.6 | 11.69x | 1668.2 | 2.25x | 5.18x |
| Bicubic Catrom | 4468.8 | 391.5 | 11.40x | 2243.3 | 1.99x | 5.73x |
| Bicubic Mitchell | 4469.1 | 404.0 | 11.01x | 2218.1 | 2.02x | 5.50x |
| Spline16 | 4462.5 | 400.9 | 11.14x | 2316.5 | 1.92x | 5.75x |
| Spline36 | 5031.9 | 525.5 | 9.58x | 3125.0 | 1.63x | 5.75x |
| Spline64 | 6044.3 | 865.5 | 6.95x | 4170.6 | 1.47x | 4.69x |
| Lanczos3 | 5053.4 | 452.9 | 11.18x | 2800.5 | 1.81x | 6.18x |
| Lanczos4 | 6012.6 | 690.4 | 8.71x | 4021.9 | 1.51x | 5.36x |
| Lanczos5 | 6724.1 | 613.9 | 11.00x | 4820.9 | 1.40x | 7.85x |
| Lanczos6 | 8421.1 | 864.3 | 9.74x | 5983.3 | 1.41x | 7.11x |

Across filters, CPU frame-level parallelism has a median `11.01x` speedup
(`6.95x..11.69x`). Current Metal has a median `1.72x` speedup over the current
single-candidate CPU path (`1.40x..2.25x`), so no retained filter reaches the
existing `>=3x` default-enable gate. Once the CPU uses frame-level parallelism,
CPU is `4.69x..7.85x` faster than current serialized Metal, with a `5.74x`
median.

The frame-parallel CPU result is a benchmark prototype, not a production API.
It combines bounded frame scheduling with one reusable `CpuWorkspace` per
worker, which is the intended verification-session execution shape.

## Profile Findings

Profiling used a separate RelWithDebInfo executable in
`build/fixed-recipe-profile`; it did not replace or rebuild the measured Release
binary above. The retained evidence is under
`build/fixed-recipe-profiles/20260731T072946Z`. Its source identity is
`dd986a1d35c1f04427546c6fb89989a9c3208c77418e5141b1c4d57de3a713ae`.

### CPU

The valid `sample(1)` profile for Lanczos6 over 1000 frames attributes the
exclusive compute samples approximately as follows:

| CPU region | Exclusive share |
| --- | ---: |
| NEON inverse solve | 43% |
| Scalar `MetricAccumulator::add` | 38% |
| Vertical reconstruction | 19% |

Disassembly shows that each four-lane NEON difference group calls the
non-inlined scalar accumulator four times, repeating threshold and norm
branches. This makes accumulator inlining/batched accumulation the clearest CPU
kernel experiment after production frame scheduling. It still requires exact
metric and whole-workspace equivalence; the profile alone does not authorize a
rewrite.

Lanczos6, 1000-frame worker sweep:

| Workers | Wall ms |
| ---: | ---: |
| 8 | 1355.1 |
| 12 | 1008.5 |
| 16 | 955.6 |

All 16 cores remain useful on this host; 16 workers are about `5.5%` faster than
12. A production session should nevertheless keep the worker limit bounded and
avoid nesting it with candidate-level pools.

### Metal

The valid Metal System Trace captured `9.634 s` of target GPU intervals in a
10-second window. Lanczos6 split-kernel GPU time is:

| Metal kernel | GPU share | Median per frame |
| --- | ---: | ---: |
| Inverse | 63.48% | 3.113 ms |
| p1 metric reduction | 36.52% | 1.800 ms |

The reduction-group sweep is the strongest measured Metal opportunity for this
load:

| Groups per candidate | 100-frame wall ms |
| ---: | ---: |
| 8 | 539.5 |
| 16 | 457.5 |
| 32 | 408.1 |
| 64 | 384.5 |
| 128 | 375.4 |
| 256 | 369.5 |
| 512 | 371.2 |
| 1024 | 370.7 |

The best observed value, 256 groups, is `1.46x` faster than the default 8-group
profile for Lanczos6. At 256 groups, inverse threadgroup sizes 16/32/64/128/256
measured `369.1/368.0/370.1/371.9/380.6 ms`; the existing 32-thread inverse
setting is already optimal within this sweep.

Three-arm 100-frame correctness runs at 256 groups passed for all 10 retained
filters. These are exploratory RelWithDebInfo results, not a Release paired A/B
approval. The value is load-specific: hard-coding 256 globally could increase
partial-buffer and merge work for 1000-candidate sweeps. The next implementation
stage should select or tune reduction groups by workload shape, then run paired
Release gates for both one-candidate-many-frame and many-candidate cases.

Do not revive B19/B23 specialization at this stage. Existing backend-hotpath
evidence found only `+3.44%` for B19 and a `-6.61%` regression for B23; reduction
fan-out has the larger measured opportunity here.

## Optimization Decisions

### CPU Frame-Level Parallelism: Adopt

All 10 retained filters select `ADOPT_FRAME_LEVEL_PARALLELISM`. The gain remains
material at 100 and 1000 frames, every result is stable, and the prototype
retains exact CPU output. This should be the first production optimization for
fixed-Recipe verification.

### Prepared Plan Residency: Reject as the Primary Optimization

The directly measured lower bound for removable plan work, Metal plan upload
plus plan-buffer allocation, is only `1.20%..2.03%` of 1000-frame Metal wall
with a `1.79%` median. All 10 filters reject plan residency as the primary
performance stage.

Plan packing is still inside host residual, so this does not prove that a
backend-neutral `PreparedCandidateSet` has no architectural value. It may still
be useful as an enabling lifetime for bounded in-flight work, but it should not
be justified by an unmeasured large standalone speedup.

### Bounded Metal In-Flight: Re-evaluate After Reduction Tuning

GPU execution is a median `81.5%` of Metal wall, source upload `4.6%`, and host
residual `12.5%`. Perfectly overlapping measured GPU and non-GPU wall gives only
an ideal `1.11x..1.41x` upper bound, median `1.23x`; all 10 retained filters clear
the `1.10x` experiment threshold.

This remains an overlap ceiling, not a measured speedup. Reduction-group tuning
changes the GPU/non-GPU balance, so remeasure the bound before building a
bounded-ring in-flight prototype.

## Deferred Post-GUI Production Optimization

Status: `DEFERRED_UNTIL_GUI_5_VERIFICATION_IS_FUNCTIONAL`.

Resume this work only after GUI-5 can execute a real typed VerificationSession
with media probe/decode, Full and Preview scopes, cancellation, resume, review,
and export. Until then, the frame-parallel scheduler and Metal tuning remain
benchmark evidence rather than delivered product acceleration.

The post-GUI baseline to preserve is:

| Path | 1000-frame throughput across retained filters | Delivery state |
| --- | ---: | --- |
| Current serial CPU | `119..266 fps`, median `198 fps` | Public engine path |
| Current serialized Metal | `167..599 fps`, median `339 fps` | Public backend path |
| CPU frame-parallel prototype | `1155..3119 fps`, median `2056 fps` | Benchmark-only scheduler |

At the prototype rate, analysis-only wall for a 24-minute 23.976 fps episode is
`16.8 s` at the cross-filter median and `29.9 s` for the slowest retained
filter. A 90-minute 24 fps feature is `63.0 s` median and `112.2 s` slowest.
These are not product wall-time promises because decode and color conversion
were unavailable in the fixture.

Implement and evaluate in this order:

1. Add one globally bounded frame-level CPU scheduler to VerificationSession,
   with one reusable `CpuWorkspace` per worker, stable frame ordering,
   cancellation, and no nested candidate or per-video worker pools.
2. Feed it from an engine-owned bounded decode/color ring and measure probe,
   decode, color conversion, queue wait, plan prepare, analysis, merge/export,
   peak memory, and total wall separately.
3. Establish backend policy from real workloads. Keep Metal for short work only
   where it wins; fixed-Recipe scans of 10 or more predecoded frames currently
   favor the CPU prototype for every retained filter.
4. If Metal remains useful, select reduction groups by workload shape and run
   paired Release gates for both one-candidate-many-frame and
   many-candidate-one-frame loads. Do not hard-code the exploratory 256-group
   result globally.
5. Only after production frame scheduling is measured, evaluate batched or
   inlined CPU metric accumulation and bounded Metal in-flight execution. Both
   must retain result identity and beat the new end-to-end baseline.

The post-GUI acceptance run must use real media and cover all-frame, decoded
I-picture, stride, custom-range, cancel/resume, and multi-video RunGroup cases.
It must report throughput and peak memory, prove no missing/duplicate/reordered
frames, retain decoder/backend provenance, and pass the existing numerical and
noise gates. Synthetic-ring results alone cannot close this item.

## Correctness and Evidence Limits

- All 50 selected Release cases pass CPU/Metal tolerance and cross-sample stability.
- Maximum CPU/Metal metric error is `1.717e-7`.
- Maximum selected 1000-frame relative MAD is `5.09%`, below the `10%` gate.
- Noise gates decisions only for 100- and 1000-frame cases. Seven of the 30
  short cases exceed the `10%` relative-MAD threshold; they remain descriptive.
- The 10 exploratory 256-group correctness runs all pass, with a maximum Metal
  error of `4.619e-9`.
- The worker, reduction-group, and inverse-thread sweep rows were captured from
  their console summaries, but their individual raw samples were not retained.
  Treat their exact millisecond values as exploratory bottleneck evidence, not
  as a reproducible Release A/B gate. The final 256-group correctness JSON and
  CPU/Metal profile captures are retained.
- No thermal or performance warning was recorded by `pmset`.
- StorageManagement background work consumed roughly 1.5-2 CPU cores during
  the Release run. Within-artifact rotated ratios and MAD gates remain useful,
  but cross-period absolute times are not clean idle-host ceilings.
- The Xcode launch-mode CPU trace stalled in dyld and was killed after 30
  seconds. It is invalid evidence; CPU hotspot conclusions use the successful
  `sample(1)` capture instead.
- The synthetic fixture excludes decode and color conversion by construction.
  Product end-to-end claims require a separate media-pipeline benchmark.

The selected result set uses original raw artifacts for eight filters and the
passing 5-sample reruns for Lanczos3/5. Nothing was overwritten; V3 references
exactly the 10 retained filter artifacts, and the checksum manifest also pins
the untouched measured Release executable.
