# D1.2 CUDA Wide-Taps: Register-Ring Inverse + Candidate Pipeline

- Date: 2026-08-08.
- Branch: `main` (follow-up to E1/E3 of `docs/dsmvc-port-strategy.md`).
- Host: Linux x86-64, AMD Ryzen 9 5950X, NVIDIA RTX 5080 (sm_120, driver
  595.84), CUDA 13.3, Release build, GCC 15.2.
- Decision: **ADOPT BOTH** (register-ring inverse fallback; 2-deep
  candidate pipeline in the worker CUDA path).

## 1. Register-ring inverse for runtime bandwidths: +77-95% wide taps

E3's ncu evidence (`e3-kernel-increments-20260808.md` §2) showed the
wide-taps falloff lives in the inverse solve. Kernel source review then
found the mechanism: the compile-time register rings only covered
half-bandwidth 3 and 5; every other bandwidth (bilinear 1, spline64 7,
lanczos8 15, …) fell back to a generic loop whose forward/backward
recurrence **re-read `output[]` from global memory every step** — a
~200-cycle L2 round trip in a loop-carried chain, 15 loads per step at
lanczos8.

Change (`getnative_cuda_baseline.cu`): `inverse_axis_ring_dynamic` and
`inverse_axis_ring_pair_dynamic` — a fixed 15-deep register window with
runtime guards, the same operation order as the generic loop (results
bit-identical by construction). Plans with band > 15 keep the old loop
(outside the backend's declared envelope).

Frozen shape (64 candidates, `both`, concurrency 16), 8 samples:

| Filter | FPS before | FPS after | Delta | inverse H ms before → after |
| --- | ---: | ---: | ---: | ---: |
| bilinear | 93.3 | 116.5 | **+25%** | 6,618 → 4,749 |
| bicubic | 162.1 | 167.2 | +3% | 3,381 → 3,541 (noise) |
| lanczos3 | 134.2 | 140.1 | +4% | 3,950 → 3,383 |
| spline64 | 42.4 | 75.2 | **+77%** | 15,100 → 8,118 |
| lanczos8 | 23.2 | 45.3 | **+95%** | 30,611 → 10,876 |

Correctness: E1-shape checksum bit-identical (`0.34542664490478819`);
full CUDA suite 13/13 incl. baseline bitwise-repeat gates and worker
cuda-parity. ncu after: inverse_horizontal lanczos8 9.83 → 8.30 ms,
registers unchanged (48/thread).

## 2. Candidate pipeline in the worker: overlap host pack with GPU

Real 301-unique-candidate scans showed the candidate phase was
host-pack-dominated (pack+upload ≈ 200 ms vs GPU ≈ 16-41 ms). The
worker now runs 32-candidate chunks through a small pipeline of threads
(2 by default; `worker_count` 1..8 overrides) onto the engine's slot
pool — chunk N's device execution overlaps chunk N+1's host pack.
Results stay in candidate order; cancel yields a contiguous-prefix
partial payload.

**Measurement-method note:** single-scan-per-process measurements are
dominated by one-time slot device-buffer allocation (~200 ms, amortizes
to zero in the resident worker). Steady-state cold-grid scans (median
of 3, fresh plan grid each scan):

| Kernel | CPU cand_ms | CUDA p1 | CUDA p2 | p4/p8 |
| --- | ---: | ---: | ---: | ---: |
| lanczos3 | 234 | 24 | **21** | 27/25 |
| lanczos8 | 271 | 64 | **65** | 75/63 |

Steady state CUDA vs CPU on unique-candidate scans: **~10x** (lanczos3)
to **~4x** (lanczos8). Pipeline depth 2 is the knee; deeper adds nothing
(host pack parallelizes across the two threads, GPU execution is small).

## 3. What did NOT work / negative results

- **More candidates per launch**: candidates/s is flat from 32 to 128
  (3,230 → 3,002 on lanczos8) — the solve is saturated at 32; 256
  exceeds the per-slot memory budget (guard works). Candidate batching
  beyond the current chunk is not the lever.
- **dsmvc's merged-solve analog does not transfer to height scans**:
  its 4.32x came from sharing one plan's loads across 8 frames; a
  height scan has a unique plan per candidate, so there is nothing to
  share. The same-plan-many-frames shape is verification (E2), whose
  CUDA path remains a documented follow-up — and verification is
  decode-bound on the CPU path anyway (E2 evidence).

## 4. Follow-ups (recorded)

- Eager per-slot device-buffer warmup at engine construction to remove
  the ~200 ms first-CUDA-job cost per session.
- CUDA verification backend (frame-merged solves = the true dsmvc
  batch analog) — only after decode lives closer to the engine.
