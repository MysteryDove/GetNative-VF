# Phase 2 entry — uProf + structure focus

Date: 2026-08-01
Profile: `uprof/typical-primary-avx2-tbp-report.csv`
Session: `uprof/session-direct-20260801-004128/try0/AMDuProf-getnative_cpu_backend_benchmark-TBP_Aug-01-2026_00-41-28`
Workload: `bicubic-catrom@810`, `--cpu-isa avx2`, samples 21 (includes P0/P1 micro + matrix)

## Caveats

- Host reports **Hyper-V VM** in uProf — timings still useful for relative hotspots.
- Call stack sampling was **off** (TBP flat functions only).
- Profile includes P0/P1 kernel microbench time, not only matrix execution.

## Top CPU_TIME functions (process total ~671 s across workers)

| Rank | Function | CPU_TIME (s) | Layer | Phase2 note |
|-----:|----------|-------------:|-------|-------------|
| 1 | `inverse_columns_x86_impl<Avx2Operations,3>` | **285.1** | ISA P0 (B7) | Already SIMD RHS; structure: better locality / less store traffic before recon |
| 2 | `add_vertical_reconstruction_row` | **167.1** | Structure + P1 dispatch | Glue + metric submit; fusion/accumulator |
| 3 | `vertical_reconstruction_avx2_f32` | **126.8** | ISA P1 | Tap FMA loop; keep plan shared; avoid extra buffers |
| 4 | `inverse_axis_impl<3>` | **35.1** | Algorithm/scalar path | Likely P0 micro or non-column path; do not expand without need |
| 5+ | ucrt/ntdll | ~40 | Runtime | Not primary |

Rough share of named hotspots (1–4):
**P0 column inverse ~45%**, **P1 recon path ~46%**, **scalar-ish inverse_axis ~6%**.

## Structure checklist vs profile

| User item | Profile implication |
|-----------|---------------------|
| Avoid re-building AxisPlan | Not in top list for *execution* path (plans prebuilt). e2e cold planner is separate (~0.3–0.5 s of wall in medians). |
| Reuse scratch | No `vector::` / alloc in top 10 — grow-to-fit OK for this shape; keep reserve-once. |
| Reduce intermediate writeback | Inverse writes full native map then recon reads it — **likely #1↔#3 coupling**; structure win if streaming/fuse inverse→metric where legal. |
| Fuse recon/residual/metric | Partially done; still large time in `add_vertical_reconstruction_row` + AVX2 recon — **metric lane scalar add is a candidate**. |
| Contiguous vs gather | Column SoA is good; plan index gathers inside inverse/recon still present. |
| Coarse parallel by candidate | 32-way workers active (many equal threads ~3.1 s) — already coarse parallel; don't add more threads blindly. |
| SIMD independent RHS | Confirmed (`inverse_columns` Avx2×8). |
| Same plan specialized/generic | B7 template `FixedHalfBandwidth=3` for bicubic — correct. |

## Recommended Phase 2 order (no over-opt)

1. **Instrument / optional second profile** with call stacks (if uProf hotspots config available) to split matrix vs P0 micro time.
2. **Metric accumulator**: reduce per-lane scalar overhead after SIMD recon (order-preserving).
3. **Streaming / less native materialization** only if proven; do not break axes/order.
4. **Do not** start AVX-512 auto or new ISA micro-kernels on this host.
5. Re-measure primary `complete_execution_ms` after each change.

## Denominator freeze (Phase 1)

| Path | complete_execution_ms median | end_to_end_ms median |
|------|-----------------------------:|---------------------:|
| auto (avx2) primary | **523.2** | **984.1** |
| forced avx2 primary | 509.1 | 844.5 |

Use **auto complete_execution** as GPU structure-fair denominator; use e2e only for full product claims.
