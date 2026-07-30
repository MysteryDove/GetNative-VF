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
| MK3 arena/ring/cache | `PENDING_EVALUATION` | PL1 is terminal; persistent allocation/upload reuse is next, while content caching still requires its own exact identity/packing checkpoint and independent gate |
| MK4 Float16 coefficient storage | `PENDING_PREREQUISITES` | Strict Float32 remains the only retained path |

## Final Identity Set

The final integrated evaluator and PL1 matrix evidence use these identities:

| Input | SHA-256 |
| --- | --- |
| Benchmark binary | `4b5651e1a1e4a9e6c68bcd9fa14a114fc9b9eea1b62feb1ed53ad1ec80c5858d` |
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
- Metal B3/F2 control: 6.73% median improvement, paired MAD 0.0115.
- Metal B7/F4 control: 8.18% median improvement, paired MAD 0.0036.
- CPU Bicubic Catmull-Rom 1080 to representative 810 candidate:
  73.39% inverse-stage improvement and 62.38% whole-candidate improvement.
- Thermal state stayed nominal, no concurrent benchmark was detected, and all
  identities remained stable.

Artifacts:

- `build/backend-hotpath-evaluator/artifacts/backend-hotpath/20260730T203330908Z-pid12708/metal-kernel-report.json`
- `build/backend-hotpath-evaluator/artifacts/backend-hotpath/20260730T203340222Z-pid12938/cpu-column-report.json`

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

## MK3 Readiness Boundary

The final Metal controls allocate 291 Metal buffers per 1000-candidate run.
Their median backend wall minus reported GPU execution time is approximately
6.9 to 8.1 ms. This residual includes packing, allocation, command encoding,
submission, synchronization, and measurement overhead; it is not direct proof
of allocation time.

The terminal Stage 1 commit `6eda3ef`, combined integration commit `00e53b9`,
and terminal PL1 decisions now satisfy the planner coordination gate. This is
sufficient to start the persistent arena and 2/4-slot upload-ring experiment.
Packed-content caching additionally requires the stable exact plan identity and
private packing-ABI checkpoint defined in the design plan; the rejected PL1
packing ABI is not retained or reused as that checkpoint.
