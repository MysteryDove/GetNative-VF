# dsmvc CUDA Recurrence Review (cross-project audit)

- Date: 2026-08-08.
- Subject: `/home/owen/dev/Descale-MVC` HEAD `8f68106`, requested after the
  same defect was found and fixed in this repo's CUDA backend (D1.2,
  `d12-cuda-wide-taps-and-pipeline-20260808.md`).
- Verdict: **the same defect exists in dsmvc**, in two device functions.

## 1. The defect

Our pre-D1.2 generic inverse fallback re-read `output[]` from global
memory at every recurrence step (a ~200-cycle L2 round trip in a
loop-carried chain, `band` loads per step). dsmvc's equivalents do the
same:

| Location | Function | Lines | Notes |
| --- | --- | --- | --- |
| `src/cuda/dsmvc_cuda.cu` | `inverse_axis` generic fallback | 325-329 (forward), 346-350 (backward) | reached for half-bandwidth ∉ {1,3,5,7} |
| `src/cuda/dsmvc_cuda.cu` | `solve_axis` generic fallback | 397-401 (forward), 421-424 (backward) | split rhs/solve design; the solve half has the same recurrence |
| `src/cuda/dsmvc_cuda.cu` | `solve_axis_row_major_tiled` generic tail | 466-470 | funnels into `solve_axis` |

## 2. Blast radius (differs from ours)

dsmvc's compile-time rings cover half-bandwidths **1, 3, 5, 7** (ours had
3, 5). Their covered set spans bilinear, bicubic, lanczos 2-4, spline16/
36/64 — and, notably, **their own published experiments stay inside it**
(the batch-8 exploration used spline64 = hbw 7, a ring path). The
generic fallback is reached only by **lanczos taps 5-8** (hbw 9/11/13/15)
within their supported envelope. So:

- getfnative-style scans that include Delanczos taps ≥ 5 on the CUDA
  backend pay the global-memory recurrence per solve step.
- Their R32T32 profile headline (2.1% occupancy, long-scoreboard stalls)
  was measured on ring-covered kernels and is a grid-size artifact of
  the VS process model, not this defect. The two issues stack for
  lanczos 5-8.

## 3. Suggested fix shape (proven in this repo)

Two options, both preserving bit-exactness (same operation order):

1. **Dynamic register ring** (what we shipped): a fixed 15-deep register
   window with runtime guards replaces the `output[]` re-reads; one code
   path covers every band ≤ 15. Our result: inverse H at lanczos8
   30.6 s → 10.9 s aggregate at the frozen shape, kernel 9.83 → 8.30 ms,
   checksum bit-identical.
2. **More instantiations**: dsmvc's bands are always odd
   (`hbw = 2·taps − 1`), so `{9, 11, 13, 15}` covers the envelope with
   exact-fit unrolled loops at a modest fatbin cost.

Either way the rhs/solve split and the slot/stream machinery are
unaffected — the change is local to the two fallback bodies (plus the
tiled tail if the tiled layout is wanted for wide bands).

## 4. Relationship to their batch ceiling

The register ring removes per-step memory latency inside one solve; it
does not raise grid-level parallelism (their 34/45-block launches at
2.1% occupancy stand on ring paths). Their author's conclusion — the
4.32x batch headroom needs a scheduler above the per-frame boundary —
remains the larger lever *inside VapourSynth*. Our repo realizes it
outside VS (worker pipeline + slot pool); the ring fix is orthogonal
and applies to both.
