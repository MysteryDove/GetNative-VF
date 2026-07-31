# Backend Hotpath Evidence

## Scope

- Branch: `perf/backend-hotpath`
- Worktree: `/Users/owen/Documents/GetNative-VF-backend-hotpath`
- Common baseline: `14890480250674bc7d6369e66ca6aaa058176ccd`
- Evaluator build: Release, strict Float32, Metal enabled, upstream conformance enabled
- Source fixture: local-only `engine/bench/fixtures/6.2-1.png`; the PNG remains
  ignored while its SHA-256 file is tracked
- Excluded: Tauri, public protocol, CUDA, Vulkan, planner reassociation, and
  public or shared-topology representation changes

## Milestone Dispositions

| Milestone | Disposition | Evidence summary |
| --- | --- | --- |
| MK0 benchmark harness | `KEPT` | Alternating same-process pairs, MAD, thermal/load/concurrency checks, atomic artifacts, full result vectors, and stable binary/metallib/matrix/fixture identities pass |
| MK1 B11/F6 | `KEPT` | Spline36 improved 13.43%; Lanczos3 improved 15.02%; generic and specialized results are bit-identical across H, V, and both two-axis orders |
| MK1 B15/F8 | `KEPT` | Spline64 improved 18.41%; Lanczos4 improved 13.91%; generic and specialized results are bit-identical across H, V, and both two-axis orders |
| MK2 B19/F10 | `REMOVED_AFTER_TEST` | Fractional improvement was 3.44%, below the 5% gate |
| MK2 B23/F12 | `REMOVED_AFTER_TEST` | A valid 810-height run regressed 6.61% |
| MK2 B27/F14 | `REMOVED_AFTER_TEST` | Valid improvement was 2.64%, below the 5% gate |
| MK2 B31/F16 | `REMOVED_AFTER_TEST` | Two valid fractional runs improved 4.83% and 4.997%, below the 5% gate |
| CPU0 AArch64 adjacent-column SIMD | `KEPT` | Scalar and NEON workspaces and metrics are bit-identical; the primary candidate and the full named matrix pass |
| PL0 exact tap reuse | `KEPT` | All 112 matrix cases are byte-identical, loop-derived raw weight calls are halved, and every valid paired case improves; Lanczos3-8 improve 31.92% to 37.18% |
| PL1 B/C geometry reuse | `KEPT` | Eight B/C sweep cases improve 10.41% to 14.22% with byte-identical plans; the single-B/C control changes +0.224%, and all MAD, thermal, concurrency, isolation, and scratch-accounting gates pass |
| PL1 packed topology interning | `REMOVED_AFTER_TEST` | Exact interning removed 77.99% of topology upload bytes but reduced total explicit working set only 6.44% and regressed primary Metal wall 1.16%, missing both alternative keep gates |
| MK3a persistent working buffers | `KEPT` | Repeated source/workspace/partial allocation falls 3 to 0; the 112-case final matrix has zero path and working-set differences, improves the primary Bicubic case 4.90%, and has no paired-median regression |
| MK3b plan-upload arena/ring | `REMOVED_AFTER_TEST` | Two/four-slot rings removed all 288 warm plan-buffer allocations and cut allocation/wiring time over 99%, but improved the primary wall only 0.689%/1.043%, below the 3% gate |
| MK3c packed-content cache | `DEFERRED_IDENTITY_CHECKPOINT` | The backend-only digest fallback must hash about 82.09 MB before every hit and has no measured headroom for the wall gate; propagating the planner's canonical key would cross the isolated planner/frontend boundary |
| MK4 Float16 coefficient storage | `REMOVED_AFTER_TEST` | Payload fell to 50% and working set declined, but Lanczos8 argmin moved 12 steps, the top-five valley identity failed, and the directional smoke improvement was only 0.957% versus the 8% gate |

## Final Identity Set

The final integrated evaluator and MK3a matrix evidence use these identities:

| Input | SHA-256 |
| --- | --- |
| Benchmark binary | `144a6c2986c4df12c01a37b1ee834c4fddd5aac394bbb8cc34985fe8a7310489` |
| Metallib | `a2e647dabf08b575c252600713eb5cf29b48e1687496088d6cb47af53ab7567c` |
| Matrix JSON | `2a34ae9224d06565191977541efcb16d1fe182b94274624cf49daa9343534b06` |
| Fixture PNG | `61f9ee1ac858bbadd6a959ba35f5eceb077b8452b91e97a5ce3d39ebc69e20c6` |
| Decoded Float32 luma | `d08b277909412a2fc33df239e377c27216f7838849be5847bf337089ffeaabdc` |

Compiler/ISA: Apple Clang 21.0.0 (`clang-2100.3.25.1`), AArch64,
`neon-f32x4`. Release disassembly of `inverse_columns_neon.cpp.o` contains
separate `fmul`, `fadd`, and `fsub` instructions and no `fmla` or `fmls`.

## Final Evaluator

Command:

```sh
cmake --build build/backend-hotpath-evaluator \
  --target getnative_backend_hotpath_evaluator
```

Result:

- 11/11 CTest cases passed.
- Metal B3/F2 control: 6.82% median improvement, paired MAD 0.007.
- Metal B7/F4 control: 8.05% median improvement, paired MAD 0.006.
- CPU Bicubic Catmull-Rom 1080 to representative 810 candidate:
  73.29% inverse-stage improvement and 61.07% whole-candidate improvement.
- Thermal state stayed nominal, no concurrent benchmark was detected, and all
  identities remained stable.

Artifacts:

- `build/backend-hotpath-evaluator/artifacts/backend-hotpath/20260731T002104755Z-pid57234/metal-kernel-report.json`
- `build/backend-hotpath-evaluator/artifacts/backend-hotpath/20260731T002113686Z-pid57462/cpu-column-report.json`

## CPU0 Full Matrix

The final-binary run covers 14 filter configurations across seven named native
heights plus the `800..899.9` fractional scan, for 112 cases with 21 alternating
scalar/NEON pairs per case.

- All 112 cases have bit-identical inverse output, whole-candidate workspace,
  and final metric hashes.
- The 111 uncontaminated cases improved whole-candidate wall time by 43.92% to
  64.75%, with a 55.25% median improvement.
- Their inverse-stage improvement was 70.90% to 73.83%.
- Maximum valid inverse/candidate paired MAD was 0.0100/0.0171.
- No valid named case regressed.

One case in the aggregate run was deliberately invalidated. During pair 1 of
`bilinear@800-899.9`, the harness detected PID 23437 running
`getnative_benchmark_support_tests` from the Pre-GPU worktree. The case was
rerun alone with the same binary and matrix hashes; it passed with 72.55%
inverse improvement, 63.24% whole-candidate improvement, MAD 0.0024/0.0027,
stable thermal state, and no concurrent process.

Artifacts:

- Aggregate: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/cpu0-final-full/20260730T193716655Z-pid22188/cpu-column-report.json`
- Clean replacement: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/cpu0-final-rerun/20260730T194019528Z-pid33737/cpu-column-report.json`

## PL0 Exact Tap Reuse

The production planner now evaluates each raw descale and zimg-forward tap once,
sums those stored doubles in the original order, and normalizes the same stored
values in the original emission order. The bounded Stage 1 builder exposes the
old recompute path only as a private diagnostic comparison mode; public and
default batch construction select reuse.

The formal matrix used 21 same-process alternating recompute/reuse pairs for 14
filter configurations across the seven named heights and the `800..899.9`
fractional scan. All 112 cases produced identical complete-plan SHA-256 values,
and focused tests compare every scalar and vector field with `memcmp` across
custom B/C, Lanczos1-8, shifts, and all border modes. Pinned descale/zimg exact
conformance also passes.

- Loop-derived raw filter-weight calls fall exactly 50% in every case.
- Lanczos3-8 valid improvements span 31.92% to 37.18%; all exceed the 5% gate.
- The 810-height Lanczos3-8 improvements are 36.87%, 32.79%, 33.66%, 31.92%,
  33.44%, and 34.63% respectively.
- Non-gated filters improve 9.26% to 28.63%; no case regresses.
- Core 1000-unique auto batch planning is 12.678 ms with a 6.936x serial/batch
  speedup. Metal batch planning is 13.110 ms; Metal execution changes -0.23%
  and phase-separated Metal total is 112.257 ms.
- A final-binary, 1 ms-interval `sample` capture records `sin` as 742
  top-of-stack samples and `ZimgKernel::weight` / `Filter::weight` as 277 / 273,
  confirming the removed evaluations target the sampled compute hotspot. The
  intentional profiler process is excluded from wall-time gate evidence.

The aggregate run retained nine invalid cases instead of accepting their raw
improvements: eight exceeded the 0.02 MAD limit, while Lanczos7 at 846 crossed
from nominal to fair thermal state. Same-binary isolated reruns of those nine
cases all passed, improving 15.96% to 32.88% with MAD 0.006 to 0.016. Thermal
state stayed nominal during every replacement, and no concurrent benchmark was
detected.

Identity and artifacts:

- Benchmark binary: `d4cbeffd48046abf8e9997fc75016b3367b329e17ee4b0e5dbbd66615c8ccecf`
- Matrix: `2a34ae9224d06565191977541efcb16d1fe182b94274624cf49daa9343534b06`
- Fixture: `61f9ee1ac858bbadd6a959ba35f5eceb077b8452b91e97a5ce3d39ebc69e20c6`
- Aggregate: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/pl0-formal-final/20260730T202644804Z-pid95108/planner-tap-report.json`
- Nine one-case replacements: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/pl0-final-replacements/`
- Sample capture: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/pl0-sample-lanczos8-final.txt`

Post-PL0 regression gates pass: Release Metal/upstream 11/11, CPU-only 9/9,
focused TSan 1/1, Metal B3/F2 and B7/F4 improve 6.73% and 8.18%, and the CPU
primary candidate improves 62.38% with bit-identical NEON/scalar results.

## PL1 Bicubic Geometry Reuse

The planner-private PL1 path groups exact geometry keys by source and
destination size, active-length bits, shift bits, and border mode. For
non-degenerate Bicubic B/C sweeps it builds the B/C-independent sampling
distances and boundary mappings once per family. Each plan still evaluates its
own B/C weights and zero mask, emits its own transpose values, forms its own
normal bands, and performs its own LDLT factorization in the original order.
`B=0,C=0` remains outside every shared family because its exact-zero weights
produce a distinct sparse shape.

The 21-pair formal matrix used 1,000 requests per case. Each sweep contained
750 exact unique requests, 250 duplicate requests, and 125 geometry families;
625 nonzero-B/C plans consumed those family geometries. Independent and reuse
mode produced identical complete-plan SHA-256 values in all nine cases, exact
duplicates retained Stage 1 pointer deduplication, and `(0,0)` remained
topologically isolated.

- The seven named heights improve 10.45% to 14.22%; the `800..899.9`
  fractional scan improves 10.41%.
- The 810-height sweep improves 10.91%.
- The single-B/C 810 control changes +0.224%, below the 3% regression gate and
  correctly builds zero geometry families.
- Maximum paired MAD is 0.0123. Thermal state stays nominal, no concurrent
  benchmark is detected, and all nine cases pass.
- Temporary geometry scratch is 16,770,500 bytes for every 125-family sweep.
  It is scoped to one batch call; PL1 adds no persistent cache and does not
  change the public `AxisPlan` representation.

Identity and artifacts:

- Benchmark binary: `4b5651e1a1e4a9e6c68bcd9fa14a114fc9b9eea1b62feb1ed53ad1ec80c5858d`
- Formal matrix: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/pl1-geometry-final/20260730T210140230Z-pid76595/planner-bicubic-geometry-report.json`
- Post-PL1 Metal evaluator: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/20260730T210417100Z-pid86634/metal-kernel-report.json`
- Post-PL1 CPU evaluator: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/20260730T210426441Z-pid86857/cpu-column-report.json`

Post-PL1 regression gates pass: Release Metal/upstream 11/11, CPU-only 9/9,
focused planner TSan 1/1, Metal B3/F2 and B7/F4 improve 6.32% and 7.95%, and
the CPU primary whole-candidate path improves 62.62% with bit-identical
NEON/scalar results.

## PL1 Packed Topology Interning - Removed After Test

The separate Metal experiment interned exact-equal transpose offsets/indices and
forward-left blocks after an O(1) geometry fingerprint and complete byte
comparison. Transpose weights, forward weights, and every LDLT factor remained
private. A diagnostic packing ABI split transpose index and weight bases so the
two paths could be compared in one binary; both paths produced identical result
SHA-256 values and zero maximum path difference.

The stable 21-pair primary `bicubic-bc@810` run found 746 exact hits among 1,000
blocks. It eliminated every equality-group duplicate and reduced total topology
upload from 23,740,948 to 5,226,248 bytes, a 77.99% reduction. That was not
enough to pass either system-level keep gate:

- Explicit Metal working set fell from 287,349,896 to 268,835,196 bytes, only
  6.44% rather than the required 10%.
- The paired wall delta median regressed 1.16% rather than improving at least
  3%; the independent/interned wall medians were 95.473/96.938 ms and paired
  MAD was 0.0068.
- GPU time rose from 88.451 to 89.364 ms and the `wall-GPU` residual rose from
  7.102 to 7.327 ms. The result was not a hidden transfer-only win.
- The no-sharing single-B/C control passed at +0.459% with MAD 0.0078. Both
  formal runs stayed thermally stable and detected no concurrent benchmark.

The interning implementation, diagnostic API, and experimental packing/shader
ABI were completely removed. Production remains at the `ac71401` PL1 geometry
baseline; the failure does not pre-approve or reject MK3 persistent allocation
and upload-ring experiments, which target different costs and retain their own
gates.

Rejected experiment identities and artifacts:

- Benchmark binary: `900305aae56a3e5965559d3a01399f11ee6dde2c6fcec4bfe717536ca8cbeece`
- Metallib: `fd774e3a6eef7aa3ca3f4ccf3180289100e47243129868715ca09be2daf39188`
- Primary failure: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/pl1-topology-formal-810/20260730T212845145Z-pid45698/metal-packed-topology-report.json`
- Single-B/C control: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/pl1-topology-formal-control/20260730T212821257Z-pid44582/metal-packed-topology-report.json`

## MK3a Persistent Working Buffers

The engine now keeps grow-to-fit shared source, workspace, and metric-partial
buffers across serialized analysis calls. The retained capacities are bounded
by a configurable 2 GiB default ceiling; a request that cannot fit the ceiling
clears the retained set and uses the original transient behavior for that call.
The transient path remains selectable for same-binary diagnostics. Source bytes
are copied into either path in the same order, and shader arithmetic, plan
packing, command order, and strict Float32 compilation are unchanged.

Telemetry separates total and working-buffer allocation counts/bytes/times,
source and plan upload time, buffer wiring time, active bytes, retained capacity
and high-water marks, and submitted/completed command counts. A submitted-command
cancellation test verifies that every command is drained before the call throws,
then immediately reuses the same engine and retained buffers successfully.

The formal 21-pair matrix covers all 14 filters at seven named native heights
plus the `800..899.9` fractional scan. The aggregate produced 100 valid cases;
one thermal-transition case and eleven MAD-over-0.02 cases were replaced with
same-binary isolated runs. The merged 112-case evidence passes:

- Bicubic Catmull-Rom at 810 has a 4.90% paired wall improvement; the
  transient/persistent wall medians are 104.966/100.926 ms and paired MAD is
  0.0059. Its GPU medians are 97.672/99.169 ms, while the `wall-GPU` residual
  falls from 7.125 to 1.738 ms.
- Bilinear at 810 has a 6.22% paired improvement and wall medians of
  74.634/69.650 ms. Across the final matrix, every paired median improves;
  improvements range from 0.010% to 8.389%, and maximum valid MAD is 0.019872.
- Every transient/persistent result path is identical, all accuracy and valley
  checks pass, and explicit working-set growth is exactly zero.
- Every measured transient call allocates three working buffers; every warm
  persistent call allocates zero and reuses all three. Submitted and completed
  command counts match in every sample.

This is an independent MK3a keep, not completion of MK3. Total repeated-call
allocation count falls only from 291 to 288 (1.03%) because each of the 32 tiles
still creates nine plan buffers. The overall 80% allocation-count and 50%
allocation/wiring-time gates remain open for the separate plan arena and 2/4
slot ring experiments.

Identity and artifacts:

- Benchmark binary: `144a6c2986c4df12c01a37b1ee834c4fddd5aac394bbb8cc34985fe8a7310489`
- Metallib: `a2e647dabf08b575c252600713eb5cf29b48e1687496088d6cb47af53ab7567c`
- Aggregate: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/mk3-working-buffer-clean-full/20260730T225239038Z-pid48988/metal-working-buffer-report.json`
- Twelve same-binary replacements: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/mk3-working-buffer-clean-replacements/`

## MK3b Plan-Upload Arena and Ring - Removed After Test

The experiment packed descriptors and all eight plan tables into one 256-byte
aligned shared Metal buffer per slot while preserving the existing buffer
indices, shader ABI, table contents, and strict Float32 arithmetic. `ring=0`
kept the nine-buffer diagnostic path. The two/four-slot paths used grow-to-fit
retained arenas under a 512 MiB configured ceiling and a 2 GiB combined retained
ceiling.

An initial whole-window drain exposed an 11.4% two-slot smoke regression because
it introduced a barrier after every two tiles. That implementation was not used
for formal evidence. The corrected experiment packed the next tile while the GPU
ran, then waited only for the exact slot about to be overwritten. Other slots
remained in flight, and cancellation drained every submitted command before
returning.

Both stable 21-pair primary `bicubic-catrom@810` runs failed the required 3%
backend-wall gate:

| Ring | Nine-buffer/ring median | Improvement | Paired MAD | Allocation count | Allocation+wiring | Plan upload median | Working-set change |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2 slots | 90.432/89.811 ms | 0.689% | 0.00392 | -100% | -99.590% | 5.231/2.014 ms | -26.54% |
| 4 slots | 90.537/89.815 ms | 1.043% | 0.00400 | -100% | -99.575% | 5.387/1.977 ms | -24.73% |

The result is compute-dominated rather than an unmeasured host win: two-slot
median GPU time changed 88.994 to 88.633 ms and four-slot changed 89.127 to
88.651 ms. Removing allocation and packing overhead therefore did not translate
into the required end-to-end gain. The full matrix was not run after both ring
depths failed this necessary primary gate.

Correctness and resource gates passed before removal: Release CTest passed
11/11; Metal API plus Shader Validation passed; all legacy/ring/fallback result
bits matched; maximum path difference was zero; all submitted commands completed;
active arena bytes returned to zero; and cancellation followed by immediate slot
reuse passed. The implementation, public diagnostic options, tests, and
benchmark mode were then removed. Production remains at the independently kept
MK3a working-buffer baseline. A clean rebuild after removal again passed Release
CTest 11/11 and Metal API plus Shader Validation.

Rejected experiment identities and artifacts:

- Benchmark binary: `44e60c653c0536bb7e2e6ee1a52b0f369852cc4b6565ff14d2ea9fc12a8acbaa`
- Metallib: `a2e647dabf08b575c252600713eb5cf29b48e1687496088d6cb47af53ab7567c`
- Two-slot failure: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/mk3-plan-ring2-primary/20260730T235034320Z-pid72550/metal-plan-ring-2-report.json`
- Four-slot failure: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/mk3-plan-ring4-primary/20260730T235115005Z-pid74349/metal-plan-ring-4-report.json`
- Experimental validation: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/mk3-plan-ring-validation/metal-validation-ctest.log`
- Post-removal validation: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/mk3-plan-ring-removed-baseline/metal-validation-ctest.log`

## MK3c Packed-Content Cache - Deferred at Identity Boundary

The transient controls allocate 291 Metal buffers per 1000-candidate run; MK3a
reduces that to 288. The remaining allocations are exactly the nine plan buffers
for each of 32 tiles. Direct telemetry for the retained Bilinear/Bicubic 810
paths reports approximately 3.0/5.5 ms of plan upload and 1.4/1.5 ms of Metal
allocation API time per call. Those counters overlap GPU execution and must not
be summed into `wall-GPU` residual, but they identify the next host-side target.

The terminal Stage 1 commit `6eda3ef`, combined integration commit `00e53b9`,
and terminal PL1/MK3a/MK3b decisions leave packed-content caching dependent on a
stable exact plan identity and private packing-ABI checkpoint. Neither the
rejected PL1 ABI nor the removed MK3b arena establishes that checkpoint.

The planner has an exact request-derived `PlanKey`, but it remains private to
`src/planner` and is not carried by `AxisPlan` or `CandidateAnalysis`. Propagating
it would change the planner/backend contract owned by the parallel planner and
frontend worktree. The only isolated Metal-backend fallback is to digest plan
content before packing and then perform the required size and byte equality
check before every hit.

That fallback has no credible performance headroom on the measured primary:
each call uploads 82,092,704 plan bytes, while local SHA-256 throughput for large
blocks is 3.413 GB/s. One digest alone therefore costs approximately 24.1 ms,
before the equality pass. By comparison, removing every warm plan allocation
and reducing plan upload by about 3.3 ms improved backend wall only
0.689%-1.043%. A digest-keyed cache has no measured headroom for the required 3%
wall improvement and would likely regress the cold and eviction-heavy gates.

MK3c is therefore `DEFERRED_IDENTITY_CHECKPOINT` in this isolated backend branch,
not implemented with pointer identity or an expensive content digest. Reopen it
only after the planner interface deliberately exposes an immutable canonical key
and the private Metal packing ABI is versioned; that future change must be
coordinated when the parallel planner/frontend branch is merged.

## MK4 Float16 Coefficient Storage - Removed After Test

The explicit, non-default experiment converted only transpose weights,
lower/upper factors, inverse diagonal, and forward weights to IEEE Float16 on
upload. Separate half-buffer entry points covered all five stages for B3, B7,
B11, B15, and generic shapes. Every shader load promoted to Float32; source,
descriptors, indices, workspace, accumulators, metric partials, and CPU merge
remained unchanged Float32. The strict engine kept its original pipeline names,
buffers, and default option.

The first `lanczos8@810` gate probe passed the storage gates but failed the
deterministic correctness gates:

- Coefficient payload fell exactly from 238,316,568 to 119,158,284 bytes, a
  ratio of 0.500. Total explicit peak working set fell from 522,093,136 to
  402,934,852 bytes, so neither the 0.52 payload ceiling nor the no-growth gate
  was the blocker.
- Float32 selected candidate 749 and ranked local valleys
  `[749, 754, 747, 739, 751]`. Float16 selected candidate 761 and ranked
  `[761, 748, 755, 729, 783]`. Exact argmin and top-five identity both failed.
- Absolute metric drift had median `1.1102e-7`, mean `1.2846e-7`, p95
  `3.0333e-7`, and maximum `4.7657e-7`. The Float16 result also failed the
  existing CPU tolerance and moved the minimum by 12 candidate steps.
- The single alternating smoke pair moved backend wall from 304.190 to
  301.280 ms, only 0.957% in the favorable direction versus the required 8%.
  GPU time moved from 301.295 to 298.461 ms, while host coefficient conversion
  cost 9.171 ms across the call. This one-pair timing is directional only and
  is not presented as a statistically accepted performance result.

One failed exact-valley fixture is a terminal MK4 remove condition, independent
of timing variance. The planned 21-pair sustained run and full filter/resolution
corpus were therefore not run after the correctness stop condition; doing so
could not make the experiment retainable.

Before removal, both Lanczos8 generic and B7 generic/specialized strict result
vectors exactly matched their pre-experiment JSON baselines. After removal, a
fresh build restored the production benchmark and metallib hashes exactly to
the MK3a baseline, Release CTest passed 11/11, and Metal API plus GPU Shader
Validation passed. The public fast-mode option, half upload path, telemetry,
benchmark mode, and all half-buffer kernels were removed; strict Float32 remains
the only production coefficient representation.

Rejected experiment identities and artifacts:

- Experimental benchmark binary: `3639ac40a83a0010ae712753724929e005224d3cd3bbd3a5617a3fce673c43e1`
- Experimental metallib: `a338395e2e13093f0d6672fc13048ee9dafbddd696989bc78ef25f8c930e42af`
- Float16 failure: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/mk4-f16-smoke-lanczos8/20260731T001629369Z-pid42616/metal-f16-coefficient-report.json`
- Strict pre/post source-change baselines: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/mk4-strict-baseline-lanczos8.json` and `build/backend-hotpath-evaluator/artifacts/backend-hotpath/mk4-strict-post-experiment-lanczos8.json`
- Restored production benchmark binary: `144a6c2986c4df12c01a37b1ee834c4fddd5aac394bbb8cc34985fe8a7310489`
- Restored production metallib: `a2e647dabf08b575c252600713eb5cf29b48e1687496088d6cb47af53ab7567c`
- Post-removal validation: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/mk4-f16-removed-baseline/metal-validation-ctest.log`
- Final Metal evaluator: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/20260731T002104755Z-pid57234/metal-kernel-report.json`
- Final CPU evaluator: `build/backend-hotpath-evaluator/artifacts/backend-hotpath/20260731T002113686Z-pid57462/cpu-column-report.json`
