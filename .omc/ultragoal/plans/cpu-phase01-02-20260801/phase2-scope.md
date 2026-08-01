# Phase 2 scope (structure-first, no over-optimization)

## Gate before code changes

1. Phase 0 baseline JSON complete for scalar/sse2/avx2/auto × 3 cases.
2. Phase 1 closed: FMA in AVX2/AVX-512 objects, `math_mode=production`, production metric tolerance, uplift table.
3. At least one uProf hotspot report for `bicubic-catrom@810` + `avx2` execution path.

## Allowed work

- Structure layer only when profile attributes ≥~10% time to that structure cost.
- Plan cache / warm residency (product path), not changing LDLT math.
- Scratch reserve-once, eliminate hot-path `vector` growth.
- Reduce intermediate frame traffic; fuse recon+residual+metric where order-safe.
- Coarse candidate parallelism tuning only if contention/oversubscription shows.
- Keep specialized B3/B7 and generic on **same** `AxisPlan`.

## Forbidden early

- ISA micro-kernels “for fun” without profile.
- AVX-512 auto-enable on this host (no ZMM).
- Formal 112 matrix as daily opt loop.
- Changing crop/threshold/candidate order to look faster.
- Parallel heavy benchmarks during measurement.

## Primary metric for wins

- `complete_execution_ms` median/MAD on `bicubic-catrom@810` (avx2/auto).
- Secondary: `lanczos8@810` regression guard.
- e2e only when claiming planner/cache improvements.
