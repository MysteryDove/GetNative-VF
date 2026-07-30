# Backend Hotpath Evidence

## Scope

- Branch: `perf/backend-hotpath`
- Worktree: `/Users/owen/Documents/GetNative-VF-backend-hotpath`
- Common baseline: `14890480250674bc7d6369e66ca6aaa058176ccd`
- Evaluator build: Release, strict Float32, Metal enabled, upstream conformance enabled
- Source fixture: local-only `engine/bench/fixtures/6.2-1.png`; the PNG remains
  ignored while its SHA-256 file is tracked
- Excluded: Tauri, public protocol, Pre-GPU Stage 0/1 implementation, CUDA,
  Vulkan, and planner arithmetic changes

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
| PL0 / PL1 | `PENDING_CHECKPOINT` | `perf/pre-gpu-stage1` has no terminal Stage 1 evidence commit; planner-owned files were not changed here |
| MK3 arena/ring/cache | `PENDING_CHECKPOINT` | The required Pre-GPU terminal evidence commit is absent; no runtime reuse implementation was started |
| MK4 Float16 coefficient storage | `PENDING_PREREQUISITES` | Strict Float32 remains the only retained path |

## Final Identity Set

The final evaluator and CPU matrix evidence use these identities:

| Input | SHA-256 |
| --- | --- |
| Benchmark binary | `407625019d3439c2a7d87b957e6f90acd61253e57c98c9936029ddaac698de6d` |
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

- 9/9 CTest cases passed.
- Metal B3/F2 control: 7.01% median improvement, paired MAD 0.0111.
- Metal B7/F4 control: 7.27% median improvement, paired MAD 0.0058.
- CPU Bicubic Catmull-Rom 1080 to representative 810 candidate:
  73.41% inverse-stage improvement and 63.31% whole-candidate improvement.
- Thermal state stayed nominal, no concurrent benchmark was detected, and all
  identities remained stable.

Artifacts:

- `build/backend-hotpath-evaluator/artifacts/backend-hotpath/20260730T193355323Z-pid5011/metal-kernel-report.json`
- `build/backend-hotpath-evaluator/artifacts/backend-hotpath/20260730T193404745Z-pid6417/cpu-column-report.json`

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

## MK3 Readiness Boundary

The final Metal controls allocate 291 Metal buffers per 1000-candidate run.
Their median backend wall minus reported GPU execution time is approximately
6.9 to 8.1 ms. This residual includes packing, allocation, command encoding,
submission, synchronization, and measurement overhead; it is not direct proof
of allocation time.

This is sufficient to keep the persistent arena and 2/4-slot upload-ring
experiment in the plan, but not to bypass its coordination gate. MK3 starts
only after `perf/pre-gpu-stage1` records a terminal Stage 0/conditional Stage 1
evidence commit. Packed-content caching additionally requires the stable exact
plan identity and private packing-ABI checkpoint defined in the design plan.
