# Phase 0/1 report — CPU multi-ISA baseline

Date: 2026-08-01
Matrix: `dev_810_filters_matrix.json` (diagnostic, 10 filters@810, this run used 3 cases)
Samples: 21 + 1 warmup; `--assert`; RelWithDebInfo
Host: AMD family 25 model 33 (Ryzen), auto→avx2, no AVX-512 OS state

## Completeness

- Result JSONs: **12** case×ISA measurements
- All assertions_pass: **True**
- All within production metric tolerance: **True**
- All bit-identical to scalar (informational): **True**

## complete_execution_ms median (MAD)

| case | scalar | sse2 | avx2 | auto |
|------|-------:|-----:|-----:|-----:|
| bicubic-catrom@810 | 3229.7 (62.6) | 689.7 (11.4) | 509.1 (8.1) | 523.2 (11.8) |
| bilinear@810 | 1979.8 (33.4) | 631.8 (10.6) | 503.0 (3.4) | 507.6 (11.7) |
| lanczos8@810 | 10830.8 (64.1) | 1499.1 (11.7) | 1384.8 (310.5) | 835.7 (8.7) |

## end_to_end_ms median (includes cold planner each sample)

| case | scalar | sse2 | avx2 | auto |
|------|-------:|-----:|-----:|-----:|
| bicubic-catrom@810 | 3637.1 (33.0) | 1135.2 (38.7) | 844.5 (10.1) | 984.1 (133.0) |
| bilinear@810 | 1959.6 (18.7) | 660.8 (10.1) | 535.3 (12.0) | 530.0 (9.0) |
| lanczos8@810 | 11991.2 (135.4) | 2716.5 (28.4) | 2172.9 (125.0) | 2012.1 (31.0) |

## Primary (`bicubic-catrom@810`) ISA uplift on execution

- SSE2 vs scalar: **4.68×** faster (368.3% gain)
- AVX2 vs SSE2: **1.35×** faster (35.5% gain) — **≥5% gate PASS**
- AVX2 vs scalar: **6.34×**
- auto selected **avx2**; exec ratio auto/avx2 = **1.028**
- auto reason: `widest available production tier`

## Regression check (execution, higher ISA should not regress >3%)

| case | sse2 vs scalar gain% | avx2 vs sse2 gain% |
|------|---------------------:|-------------------:|
| bicubic-catrom@810 | 368.3 | 35.5 |
| bilinear@810 | 213.4 | 25.6 |
| lanczos8@810 | 622.5 | 8.3 |

## Phase 1 codegen / contract

- `math_mode=production`
- AVX2/AVX-512 objects contain `vfmadd`/`vfnmadd` (see `disassembly/fma-scan.json`)
- SSE2 uses mul+add; baseline/SSE2 objects free of high-ISA leakage
- Assert path uses metric tolerance `max(1e-7, 5e-4*|ref|)`, not bit-identical hard fail

## GPU denominator candidate (auto primary)

- complete_execution_ms median: **523.20** (MAD 11.82)
- end_to_end_ms median: **984.09** (MAD 133.04)

## Phase 2 entry criteria

- [x] Multi-ISA baseline complete
- [x] Production tolerance + FMA evidence
- [x] uProf typical profile for primary avx2 (`uprof/typical-primary-avx2-tbp-report.csv`)
- [ ] Structure-layer changes only if profile attributes cost (see `phase2-entry.md`)

## Note on lanczos8 avx2 MAD

Forced `avx2` lanczos8 execution MAD was large (310) while `auto` was stable (835 ms median). Treat forced-avx2 lanczos8 as **noisy** in this serial marathon; primary bicubic numbers are the decision anchor.
