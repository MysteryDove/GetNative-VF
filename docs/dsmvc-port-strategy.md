# dsmvc Port Strategy and Scheduling-First Optimization Directions

- Status: Planning baseline for the next engine lane.
- Landed: E0 (worker protocol v1, `docs/worker-protocol-v1.md`), E1 (CUDA
  session path + source residency,
  `docs/performance/e1-cuda-batch-scheduling-20260808.md` — see its §7 for
  the corrected same-session delta), E2 (verify streaming v1.1,
  `docs/performance/e2-verification-pipeline-20260808.md`), E3 (metric
  fusion adopted; large-taps CUDA per-kernel rewrite rejected — headroom
  is candidate-batched merged solves, D1.2;
  `docs/performance/e3-kernel-increments-20260808.md`), E4 (L1
  single-flight LRU default + L2 pack store opt-in,
  `docs/performance/e4-cold-plan-store-20260808.md` — hit-rate criterion
  met; store loses to the parallel build on 16-core NVMe hosts, hence
  opt-in), D1.2 (register-ring inverse +77-95% wide taps, 2-deep
  candidate pipeline, steady-state CUDA scan 4-10x CPU;
  `docs/performance/d12-cuda-wide-taps-and-pipeline-20260808.md`), D1.3
  (pack-into-pinned scan host path, pipeline depth 3; lanczos8 h_plus_w
  candidate phase 222→162 ms;
  `docs/performance/d13-cuda-scan-host-path-20260808.md`).
- Date: 2026-08-07.
- Audience: future engine/UI lane owners.
- Sources: `/home/owen/dev/Descale-MVC` (`dsmvc`, VapourSynth API4 descale
  rewrite, HEAD `8f68106`), this repository at merge `af48396`.
- Related: `docs/architecture.md`, `docs/performance/*`,
  `docs/worktree-handoff.md`, `docs/gui-development-spec.md`.

## 1. Thesis

Both projects independently converged on the same mathematics: Float64 CSR
normal-equation assembly, banded LDLT factorization with epsilon pivot,
Float32 immutable solve coefficients, bit-exact plan keys, and geometry reuse
across Bicubic `(b, c)` families. The numeric and SIMD/CUDA kernel layers are
therefore **closed** in this engine, with evidence (`docs/performance/*`,
`docs/backend-hotpath-evidence.md`).

`dsmvc` then proved where the remaining ceiling lives. It optimized E2E
throughput inside VapourSynth to its practical limit — 13.1x over the
original descale at R1T1 — and its own profiles show that the residue is not
compute but **scheduling imposed by the VapourSynth process model**: a pull
model that delivers one frame per call, requires host-resident output, and
forces a hard GPU synchronization at every frame boundary. CUDA never gets
ahead of the CPU backend there despite a measured 4.32x kernel-level batch
headroom.

GetNative-VF owns its process. The fixed analysis workloads (fixed kernel
set, fixed metric contract, batch candidate grids, whole-video scans) let us
build the scheduler dsmvc could not. **Scheduling and structure are the
first-order levers; kernel work is incremental.** This document records the
evidence, the workload shapes, and the prioritized directions.

## 2. The VapourSynth process-model ceiling

### 2.1 The contract

`dsmvc` is a standard API4 asynchronous filter
(`src/vs_plugin.cpp:909-913`, `fmParallel` + `rpStrictSpatial`). Within VapourSynth:

- The thread pool belongs to the VS core; request concurrency and order are
  pull-driven (`--requests N`, core thread count). The plugin reacts; it
  cannot prefetch, reorder, or push.
- `getFrame` is a one-frame-in, one-frame-out contract, and the output must
  be a **host-memory** VSFrame. Every `execute_2d` therefore ends in a hard
  `cudaStreamSynchronize` (`src/cuda/cuda_executor.cpp:2226-2231`).
- Output frame caching is VS-default unless the plugin uses API4 cache
  primitives (dsmvc does not); users must hand-tune `core.max_cache_size`
  or VS retains GiBs of host frames independently of the CUDA caches.
- A getnative-style scan is not a graph: ~30,800 candidates mean ~30,800
  Python `FrameEval` callbacks and ~30,800 filter instances
  (`docs/release-benchmark.md` in Descale-MVC).
- Each benchmark unit spawns a fresh VSPipe process (~1.5 s process
  overhead per unit, CUDA context re-creation included).

### 2.2 Measured cost of the contract

From `profile-results/cuda-r32t32-short-20260806` (RTX-class GPU, R32T32):

- GetNative 10,000-candidate scan, 27.10 s wall: `cuStreamSynchronize` =
  **47.9%** of CUDA API time (19.96 s / 20,008 calls); HtoD 24.0%
  (20,000 calls = 2 packed-plan uploads per candidate); DtoH totals
  **49.57 GB** of statistics output alone.
- BlankClip 5,000-frame stream, 7.25 s wall: `cuStreamSynchronize` =
  **88.0%** (10.18 s / 10,008 calls ≈ 2 per frame); HtoD 41.47 GB +
  DtoH 23.33 GB.
- After the 8f68106 staging optimization, sync calls halved
  (10,008 → 5,005) but wall time improved only 0.8% — the remaining
  per-frame synchronization latency itself is the floor.
- Nsight Compute on the split-solve kernels: 34/45 blocks, **~2.1%
  occupancy**, long-scoreboard stalls, DRAM throughput < 3.3%. The GPU is
  starving, not computing.
- Batch=8 experiment (`BATCHING-EXPLORATION.md`): **4.32x kernel-level
  speedup** (838 µs → 194 µs per merged solve), occupancy rises to
  6.7/8.9%, DRAM still < 13%. Both integration prototypes (pointer batch,
  full-frame batch) failed to convert this into wall time inside the
  synchronous per-frame call.
- Net effect: CUDA GetNative R32T32 = 341 → 409 candidates/s after months
  of work, versus **CPU backend 361 c/s** on the same host. CUDA BlankClip
  ~695-701 fps versus CPU ~1038-1054 fps. The GPU backend cannot beat the
  CPU backend under the VS contract.

The author's own conclusion (`BATCHING-EXPLORATION.md:72-83`):

> "it should not be implemented inside the current synchronous `execute_2d`
> call. A useful implementation needs a scheduler above that boundary…
> That is effectively a GPU-resident VapourSynth pipeline, not another
> executor micro-optimization."

And on the API4 migration notes for GetNative: "it needs a shared
runtime-level batch queue rather than per-filter linear caching."

### 2.3 Scaling-collapse evidence

- getfnative scan: R1 13.569 → 178.114 c/s (**13.13x**); R32 55.581 →
  358.623 c/s (**6.45x**). At R1 the win is plan caching + AVX2; at R32
  both plugins share the same denominators (VS frame plumbing, FrameEval,
  PlaneStats, process overhead, and a saturated ~44-46 GB/s memory
  hierarchy).
- dsmvc itself saturates at R8: BlankClip R8T8 = 1210-1215 fps, R16/R32
  regress to ~1040 fps. More VS request concurrency past ~8 only adds
  contention. R1→R32 amplification is 2.02x — Amdahl's serial/framework
  share dominates the high-concurrency regime.
- API4 `rpStrictSpatial` alone cut upstream-attributed time at R1T1 from
  2.11-2.16 s to 0.51-0.66 s — a measure of how much pure frame-lifecycle
  framework cost exists even before the plugin runs.

### 2.4 dsmvc's in-VS compensations, and their fate in our process

| dsmvc mechanism (evidence) | Compensates for | In GetNative-VF |
| --- | --- | --- |
| SingleFlightLru plan/geometry caches, process-wide statics (`axis_plan.cpp:102-244`) | ~30,800 filter instances rebuilding identical plans | Native: one session cache owned by the resident worker |
| Lazy planning + `call_once` per instance (`vs_plugin.cpp:540-570`) | Planning at graph-build time is wasted; multi-thread first-frame races | Native: plan at Run start, once |
| CUDA InputCache keyed on host frame pointer + `addFrameRef` keepalive (`cuda_executor.cpp:943-1052`, `vs_plugin.cpp:645-656`) | Thousands of candidates re-upload the same source frame | Replace pointer key with frame identity/generation; no keepalive hack |
| Decoupled pinned staging arena + staging semaphore (`cuda_executor.cpp:1324-1341`) | Host pack/unpack trapped inside GPU slot lifetime | Keep the design; depth becomes a tuning knob |
| ExecutionSlot pool with class-based throttling (4→8 slots) | N VS threads entering execute_2d concurrently | Keep; driven by our scheduler instead of VS arrival |
| MemoryPhaseLimiter (`vs_plugin.cpp:145-193`) | Too many concurrent working sets evicting each other at R32 | Delete: we choose concurrency deliberately |
| ActiveFrameGuard dual path (`vs_plugin.cpp:667-681`) | Cannot know how many frames VS has in flight | Delete: pipeline depth is explicit |
| CPU WorkerPool opportunistic single-flight, ≤4 threads (`cpu_executor.cpp:31-145`) | R1 wants intra-frame parallelism, R32 wants it gone | Replace with our own scheduler; thread count is ours |
| NVTX range attribution (33 labels) | — | Port directly (`cuda/nvtx.hpp` is VS-free) |

Mechanisms that disappear entirely in our process: per-frame GPU sync
boundary; per-frame HtoD/DtoH round-trips for verification (frames can stay
device-resident decode→descale→metric); VS frame pool protocol and
`newVideoFrame`/`freeFrame` churn; two-stage `arInitial`/`arAllFramesReady`
callbacks and FrameEval instance churn; three stacked schedulers that do not
know each other (VS pool ≤32 + plugin pool ≤4 + CUDA slots ≤8 + decoder
threads); per-process CUDA context cold start (resident process amortizes
once); Python wrapper conversions (`dsmvc.py`, `convert_and_invoke` double
frame copies, `VSFunction` custom-kernel callbacks).

## 3. GetNative-VF workload taxonomy (工况)

Shapes from `app/src/engine/heightDraft.ts`, `app/src/engine/candidateGrid.ts`,
`app/src/engine/runGroupPlan.ts`, `docs/performance/*`.

| Workload | Shape | Plan structure | Bottleneck profile |
| --- | --- | --- | --- |
| Height scan (分辨率测试) | Default integer coarse: heights 500-800 step 1 = **301 candidates**, 1 kernel, per included Sample; hard cap 10,000; refine = ±1.0 step 0.1 = **21 points** | Each height = unique destination_size = unique PlanKey. Same frame, same kernel family; B/C geometry reuse applies for bicubic. ~94.4 KiB/plan; 1,000 plans ≈ 92.2 MiB | Plan build throughput (6.65x batched already), then candidate apply; RHS (source frame) shared across all candidates |
| Kernel scan (算法测试) | Fixed geometry, many kernels incl. Bicubic (b, c) grid | Unique PlanKey per kernel parameters; geometry shared within a support family | Same as above, shifted toward plan building |
| Full verification (全视频检查) | 1 locked Recipe, every eligible frame; 24-min episode ≈ **34,500 frames** | **One plan set, fully warm**: cold→warm 16.97 → 0.059 ms (291.78x) | Decode + per-frame apply + metric reduction; frame-parallel CPU prototype already 11.01x serial, 2,056 fps median (≈16.8 s analysis-only per episode) — I/O and decode will dominate once adopted |
| Preview scan (I 帧检查) | Same Recipe, I-pictures or every-N frames | Same as verification, fewer frames | Same, decode-skewed |

Cross-cutting reuse facts:

- RunGroup members (samples × kernels) share the candidate grid and metric;
  a session-lived cache makes cross-member plan reuse free. The current
  one-shot CLI makes every member a cold process — nothing persists.
- `AxisPlanCache` is caller-owned, 1024 entries / 256 MiB, fixed admission,
  explicitly **not single-flight** (`engine/include/getnative/axis_plan.hpp:81-84`).
  Lanczos6/7/8 1,000-plan batches hit only 92.7/79.7/69.8% under the byte
  cap — admission policy is a real lever for large-taps scans.
- CUDA slot plan identity is pointer-based (`cuda_backend.cpp:727-774`):
  cross-process reuse is impossible by construction.

## 4. Optimization directions, prioritized

### D0 — Resident worker session (protocol v1). First.

Everything else needs a host that outlives one call. The semantic protocol
already exists (`docs/architecture.md:317-336`, `app/src/engine/protocol.ts`:
hello/capabilities/probe/analyze/cancel/export/shutdown +
accepted/progress/warning/result/cancelled/error). Wiring the engine CLI to
a JSONL worker loop converts:

- `AxisPlanCache` from dead code into a real session citizen (cross-member,
  cross-frame retention, 291.78x warm factor materializes);
- CUDA slot/workspace/plan residency from per-process to per-session;
- cooperative cancellation and progress events (GUI-3/5 hard requirements);
- engine startup/context costs from per-command to once per session.

This is also the GUI-3 unblock; the UI lane is gated on
`commands.analyze=true`, which only makes sense on the worker protocol.

### D1 — Candidate batch scheduler above per-candidate execution. The CUDA prize.

Realize what `BATCHING-EXPLORATION.md` proved and could not cash: for one
source frame and N candidates sharing it (height scan: N=301 default;
kernel scan: N=(b,c) grid), schedule on device:

- upload the frame once (dsmvc InputCache proved the single-upload pattern;
  we key it by frame identity, not pointer);
- batch candidates into double-buffered device batch slots; overlap
  candidate N+1 weight/plan upload with candidate N solve (dsmvc's
  plan_stream + event handoff, `cuda_executor.cpp:1452-1496`, is the exact
  pattern, including the hot-path atomic short-circuit);
- keep intermediate H/V results device-resident between passes; reduce
  metrics on device; DtoH carries scalars/small maps only, not frames;
- flush partial batches without timeouts at grid end.

Kernel-level headroom measured by dsmvc: 4.32x on merged solves at batch 8,
with DRAM < 13% — the budget is latency/occupancy, not bandwidth. Our
current CUDA staged baseline (frozen policy `i64-p256-m128-vpair`,
8,406 c/s at 64 Lanczos-3 candidates on RTX 5080) gives the per-candidate
floor; the batch scheduler is the multiplier. Target evidence: candidates/s
per kernel family on the 1,000-point fixture versus the frozen baseline;
Nsight occupancy report attached.

### D2 — Verification frame pipeline. Decode → analyze overlap.

Adopt the frame-parallel CPU prototype (16 workers, 11.01x median over
serial, beats serial Metal 4.69-7.85x — currently benchmark-only,
`docs/performance/fixed-recipe-multiframe-results.md`) into the product
path behind the worker protocol, then structure verification as a
bounded-queue pipeline: ffmpeg decode (existing Tauri-side media.rs paths,
or engine-side decode later) → pinned staging → analysis workers → metric
append. At the prototype's 2,056 fps median, a 34,500-frame episode is
~16.8 s analysis-only, so decode and I/O become the wall — pipeline depth
and backpressure, not more compute. CUDA verification reuses D1's
device-resident stream per Source.

### D3 — Kernel-level increments (after D0-D2 land).

- **SIMD metric accumulator**: 38% of the Lanczos6 1,000-frame CPU profile
  is scalar `MetricAccumulator::add` (`cpu_analysis.cpp:146-160`); NEON
  inverse is 43% and already done. Clearest next CPU kernel win.
- **Large-taps CUDA family**: per-family FPS falls hard (Bilinear 312.3,
  Lanczos3 278.2, Spline64 165.2, Lanczos8 100.2). Investigate with the
  autotuner contract (`cuda-autotune-contract-20260803.md`) before any
  kernel rewrite.
- **Plan cache policy**: fixed admission loses 7-30% on Lanczos6-8 scans;
  dsmvc's SingleFlightLru (byte+entry dual-cap LRU, build-outside-lock,
  failure propagation) is the reference design to adopt when the session
  owner exists.
- **CPU packed plans**: dsmvc's `PackedCpuPlan` (CSR → dense sliding-window
  rows, 8-aligned padding, canonical sharing via weak_ptr registry +
  prepare/seal) versus our CSR + row-major AVX2 inverse-column
  specialization. Benchmark both on the formal matrix before adopting; ours
  already has per-shape dispatch (`inverse_columns_avx2.cpp:69-84`).

### D4 — Persistent plan store (cold cache), with measured codec

`docs/cold-plan-cache-evaluation.md` evaluates this fully. Plans are
content-independent of image pixels (the key is geometry + kernel only), so
repeated same-resolution workloads reuse 100% of plans across images and
process restarts. The win is not throughput (a 1,000-plan batch builds in
15.81 ms) but removing the measured fixed-admission cliff: 1,000-plan
Lanczos-6/7/8 scans currently force 7-30% rebuilds. Design: L1 memory
single-flight LRU (adopt dsmvc SingleFlightLru semantics) + L2 chunked
packs — one file per candidate grid, plans grouped 64-per-chunk as
independent zstd1 frames behind a sorted key-hash index, with a lossless
structural pre-transform (uniform forward offsets dropped, forward
indices reduced to per-row anchors, CSR arrays delta-varint). Measured on
real corpora: 4.63x/4.07x (bicubic/spline64) at ~3 GB/s decompress;
sparse single-plan reads cost ~0.4 ms (one ~0.6 MiB chunk), a
30,000-candidate grid is ~640 MiB on disk and preheats in ~0.25 s. Build
fingerprints gate cross-build loading; corrupt packs always degrade to
rebuild.

### D5 — Explicitly not now

CUDA packed batch persistence (device layouts are internal and
version-sensitive); persistent cudaHostRegister of frame buffers (revisit
when decode lives engine-side); AVX-512 approval matrix (unchanged
policy); trained zstd dictionaries for the plan store (grid packs already
beat them, §3.1 of the evaluation).

## 5. Port inventory

| Asset (dsmvc location) | VS coupling | Verdict |
| --- | --- | --- |
| Planner: CSR/LDLT pipeline, two-level exact keys, SingleFlightLru (`axis_plan.cpp`) | None | **Idea-port only where better**: our planner already matches the math and has batch build + diagnostics; adopt SingleFlightLru policy and irrelevant-parameter key normalization if benchmarks demand |
| Scalar reference solver `inverse_axis_f32` | None | Skip (ours exists) |
| PackedCpuPlan layout + canonical sharing (`cpu_packed.hpp`, `cpu_executor.cpp:280-404`) | None | Benchmark against our row-major AVX2 path, then decide |
| AVX2 kernel family (8x8 transpose row solve, b1/b3 specializations, L2 32-column blocking, fused integer conversion) | None | Port ideas selectively via formal-matrix A/B |
| WorkerPool (barrier, ≤4 threads, 262,144 work threshold) | None | Do not port; design our scheduler instead |
| CUDA kernels (ring recursion, tiled row-major, transpose, rhs/solve split) | None | We have equivalents with autotuned launch policy; A/B per family before touching |
| ExecutionSlot pool, DeviceArena/PinnedBlockPool/DeviceEventPool, plan_stream event handoff, 4 HostTransferModes, NVTX | None | **Direct port candidates** for D1 — this is the scheduling blueprint dsmvc validated |
| InputCache pointer-key + keepalive | Thin | Port idea with frame-identity key |
| `vs_plugin.cpp`, `dsmvc.py`, chroma derivation, integer range conversion, custom-kernel bridge | Deep | Do not port |
| Plan disk format | Does not exist | Design ourselves if ever needed |

### Porting hazards

- Border-mode enums differ between dsmvc.py (`MIRROR=0/ZERO=1/REPEAT=2`)
  and its C++ (`zero=0/repeat=1/mirror=2`) — never copy enum values across
  boundaries without the explicit mapping.
- Bit compatibility with descale depends on Float64 accumulation order in
  `form_normal_bands` and the `round_half_up` epsilon
  (`floor(x + 0.49999999999999994)`). Both projects replicate it; keep the
  upstream conformance tests (`engine/tests/upstream_conformance_test.cpp`)
  green through every port.
- dsmvc CUDA kernels carry a "zero stack / zero local memory" invariant
  enforced by `verify_cuda_fatbin.cmake`; our baseline shares the
  invariant — keep both verifiers when merging ideas.

## 6. Roadmap and evidence discipline

Same lane rules as `docs/worktree-handoff.md`: dedicated branch + worktree
per lane, evidence artifacts committed (`docs/performance/*`, benchmark
JSON), keep/reject decisions recorded with numbers, upstream conformance
and ctest green before merge.

| Phase | Deliverable | Exit evidence |
| --- | --- | --- |
| E0 Resident worker (D0) | JSONL worker protocol v1 in engine CLI + Tauri transport; `commands.analyze` flips true per backend contract | Session-spanning plan cache hit-rate log; cancel/progress events; GUI-3 unblocked end-to-end on one Sample |
| E1 Candidate batch scheduler (D1) | Device-resident batch execution for height/kernel scans | c/s per kernel family vs frozen baseline on the 1,000-point fixture; Nsight occupancy; memory ceiling respected (`cuda_memory_policy.hpp`) |
| E2 Verification pipeline (D2) | Frame-parallel product path + decode/analyze overlap | Episode-scale fps vs 2,056 fps prototype baseline; GUI-5 acceptance scenarios |
| E3 Kernel increments (D3) | Metric SIMD; large-taps CUDA study; cache policy A/B | Per-item keep/reject notes with benchmark JSON |
| E4 Persistent plan store (D4) | L1 single-flight LRU + L2 zstd1 grid packs | Hit rate 79.7/69.8% → ~100% on repeated 1,000-plan Lanczos-6/8 scans; preheated vs cold first-scan latency |

Phase ordering rationale: E0 is structural and unblocks the UI lane; E1 is
where the largest measured headroom lives (4.32x kernel ceiling, currently
0% realized); E2 converts an already-proven 11.01x prototype; E3 is
incremental and safe to interleave.

## 7. Open questions

- Worker transport: stdin/stdout JSONL vs local socket — protocol.ts is
  transport-agnostic; decide with the Tauri lane.
- Where decode lives long-term (Tauri media sidecar today; engine-side
  decode would complete the device-resident pipeline for CUDA
  verification).
- Whether the CPU packed-plan layout from dsmvc beats our CSR + row-major
  specialization at production shapes — formal-matrix A/B required.
- ~~Session cache limits per workload class~~ — resolved by D4: L1 LRU
  plus the L2 pack store; eviction policy per the evaluation document.
