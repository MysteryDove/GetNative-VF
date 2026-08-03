# CUDA Autotune Workload Contract - 2026-08-03

## Objective

`getnative-cuda-autotune` has three explicit measurement modes. Double-clicking
the executable, or running it without a mode flag, starts the representative
`survey` protocol. `--standard` retains the complete 36-member
`launch_policy_v1` search and is the only mode that can produce a
production-admissible recommendation. `--quick` remains a report-format and
correctness smoke test.

The tool searches launch geometry that is robust across supported filter
topologies. It does not search caller concurrency, power limits, clocks, CUDA
binary implementations, inline PTX, or filter-specific production dispatch.

The frozen production launch policy is `i64-p256-m128-vpair`
(`inverse_threads=64`, `pixel_threads=256`, `maximum_metric_blocks=128`, and
`paired_vertical=true`). Survey and standard benchmark candidates are evaluated
against this policy; benchmark output cannot replace it without the explicit
standard-mode decision gates below.

Concurrency remains a fixed property of each workload. The contract contains
both serial and four-caller cases because both are real backend call shapes,
not because the tool is trying to discover the concurrency required to
saturate a particular GPU. Before ranking starts, the production baseline is
used once to calibrate sample lengths for the device. Those iteration counts
are then locked and shared by every policy; calibration never changes
concurrency or contributes to a score.

## Target Architecture Coverage

The distributable embeds native code only for `sm_75`, `sm_86`, `sm_89`, and
`sm_120`. It also embeds `compute_75` and `compute_120` PTX to satisfy the
backend's compatibility-floor and forward-image requirements. The benchmark
enforces an exact runtime whitelist for the four target compute capabilities,
so PTX availability does not admit results from unselected architecture
families.

## Filter Topology Coverage

Filters with the same support execute the same instruction and memory-access
shape after the AxisPlan has been built. Coefficient values differ, but planner
construction is outside the timed region. The contract therefore uses one
named representative per useful topology instead of timing every alias.

| Topology | Representative | Equivalent examples | Support | Half-bandwidth | Forward width | CUDA inverse path |
| --- | --- | --- | ---: | ---: | ---: | --- |
| B2 | Bilinear | Lanczos 1 | 1 | 1 | 2 | runtime generic |
| B4 | Bicubic | Spline16, Lanczos 2 | 2 | 3 | ring specialized |
| B6 | Lanczos 3 | Spline36 | 3 | 5 | ring specialized |
| B8 | Spline64 | Lanczos 4 | 4 | 7 | runtime generic |
| B16-wide | Lanczos 8 | supported wide-path boundary | 8 | 15 | 16 | runtime generic |

B2, B8, and B16-wide close the previous correctness and performance gap in
the runtime generic inverse loop. B16-wide also reaches the CUDA backend's
declared `half_bandwidth <= 15` and `forward_width <= 16` boundary. Intermediate
Lanczos widths use the same runtime loops and are bounded by B8 and B16-wide;
adding every width would substantially lengthen a double-click run without
adding a new implementation branch.

## Default Survey Measurement Set

Survey mode evaluates 14 policies: the full inverse-thread/vertical-pair group
at pixel 256 and metric 128, the full pixel/metric group at inverse 64 with
paired vertical enabled, and three known filter-family interactions. Duplicate
production-baseline entries are removed. This set contains the leaders observed
for all five filter families in the prior exhaustive `sm_120` run while still
varying every launch-policy dimension.

Eight 1080p workloads preserve the implementation branches and call shapes
that matter most:

| Workload | Candidates | Callers | Weight |
| --- | ---: | ---: | ---: |
| B2 combined | 8 | 4 | 0.08 |
| B4 combined | 8 | 4 | 0.15 |
| B6 combined | 8 | 4 | 0.15 |
| B8 combined | 8 | 4 | 0.08 |
| B16-wide combined | 8 | 4 | 0.08 |
| B6 combined batch | 64 | 1 | 0.25 |
| B6 vertical | 48 | 1 | 0.105 |
| B6 horizontal | 48 | 1 | 0.105 |

Survey screening uses two samples and confirmation uses five samples with at
most two finalists. Baseline-before/baseline-after bracketing and all
correctness/resource checks remain active. Calibration uses shorter wave
targets than the standard protocol:

| Representative | Screening target | Confirmation target |
| --- | ---: | ---: |
| Bilinear | 100 ms | 300 ms |
| Bicubic | 100 ms | 350 ms |
| Lanczos 3 | 120 ms | 425 ms |
| Spline64 | 140 ms | 525 ms |
| Lanczos 8 | 180 ms | 675 ms |

Survey output is evidence-only. It always leaves `safe_to_apply=false`, labels
overall and family results as `survey_only`, and directs policy promotion to an
explicit `--standard` run.

## Explicit Standard Measurement Set

Screening evaluates nine workloads for all 36 policies. Bicubic and Lanczos 3
retain 720p combined, 1080p vertical, and 1080p fixed-four-caller cases. The
B2, B8, and B16-wide representatives add one sustained 1080p combined case
each.

Confirmation evaluates eleven workloads. Bicubic and Lanczos 3 also include
the 1080p horizontal path; each generic representative has one 1080p combined
headline. At most four unique policies advance, selected from the two global
leaders and filter-family leaders. This cap prevents the confirmation cost
from growing with the number of reported filter families.

The static workload definitions provide iteration upper bounds. Per-device
calibration uses a four-iteration-per-worker timing probe and topology-aware
wave targets:

| Representative | Screening target | Confirmation target |
| --- | ---: | ---: |
| Bilinear | 160 ms | 650 ms |
| Bicubic | 160 ms | 800 ms |
| Lanczos 3 | 180 ms | 900 ms |
| Spline64 | 220 ms | 1300 ms |
| Lanczos 8 | 300 ms | 1900 ms |

Screening keeps at least four iterations per worker and confirmation keeps at
least eight. Fast devices therefore receive enough work to suppress launch and
host timing noise, while slower devices do not inherit an excessive fixed
frame count chosen on a high-end GPU. Confirmation uses seven independent
samples and baseline-before/baseline-after bracketing, yielding fourteen
baseline samples for every workload.

Ranking uses median wall time for the complete backend API call. CUDA events
provide stage diagnostics only; hardware counters are not required. The JSON
protocol records the uncalibrated upper-bound contract, the calibration rule,
and the exact effective iteration count in every device workload evaluation.

## Result Semantics

One frame is one completed `analyze_axis_batch_f32` call, including host pack,
source staging, H2D upload, CUDA execution, result readback, and execution-slot
contention. The visible score is sustained backend E2E FPS under the workload's
fixed caller count. The JSON also reports `candidate_analyses_per_second` as
`FPS * candidate_count`; it must not be confused with image FPS.

Quick mode is a correctness and report-format smoke test. Survey mode is a
representative architecture survey. Neither can recommend a production policy.
A standard result is admissible only after CPU agreement, bitwise-repeat,
zero-local-memory, noise, baseline-drift, minimum-gain, and
per-workload-regression gates all pass. Failure retains the production baseline
and never modifies the runner's configuration.

## Current Default Survey Verification (v1.4.0)

The final no-mode-flag survey was verified on an RTX 5080 (`sm_120`). Protocol
elapsed time was 179.884 seconds, 67.0% below the prior 545.39-second standard
run. It completed 14 policy evaluations over eight screening workloads,
advanced two finalists, and retained five confirmation samples with
baseline-before/baseline-after bracketing.

CPU agreement, bitwise repeatability, and all nine zero-local-memory kernel
resource gates passed. The result was correctly labeled `survey_only` with
`safe_to_apply=false`. Maximum baseline drift was 7.08%, which is preserved in
the report and reinforces why the shortened survey cannot promote a policy.
The final artifact is 6,324,736 bytes and has SHA-256
`56e82d897384619b7bd514d5bb9db10c1ca4c7c00f69bef3b551eea6dcb2489a`.
Its inventory contains exactly native `sm_75`, `sm_86`, `sm_89`, and `sm_120`
images plus `compute_75` and `compute_120` PTX, imports only `KERNEL32.dll`, and
has no dynamic MSVC runtime dependency.

## Prior Standard Verification (v1.3.0)

The exported v1.3.0 executable was verified on an RTX 5080 (`sm_120`) using the
standard contract. The complete run took 545.39 seconds, advanced four unique
finalists, retained identical calibrated iteration counts across every policy,
and measured 2.98% maximum baseline drift. The overall baseline remained
selected because the best confirmed composite gain was only 0.67%.

| Representative | Baseline FPS | Observed-leader FPS | Family composite | Maximum relative MAD | Decision |
| --- | ---: | ---: | ---: | ---: | --- |
| Bilinear | 312.3 | 310.3 | -0.63% | 1.56% | retain baseline |
| Bicubic | 347.8 | 346.0 | +0.74% | 1.28% | retain baseline |
| Lanczos 3 | 278.2 | 277.2 | +0.88% | 1.02% | retain baseline |
| Spline64 | 165.2 | 170.5 | +3.21% | 3.10% | retain baseline |
| Lanczos 8 | 100.2 | 101.0 | +0.77% | 2.37% | retain baseline |

Spline64 illustrates the conservative gate: its observed headline was faster,
but `2 * maximum_relative_MAD` raised the required gain to 6.19%, so a 3.21%
composite observation was not promoted. The distributable is 18,550,784 bytes,
imports only `KERNEL32.dll`, and has SHA-256
`bdc66c45c8a464c5b07b8883459de59ea467bc81a0f91339ff6f4ef5f95467d7`.
