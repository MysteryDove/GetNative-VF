# CPU stack layer map (as-is vs target)

Date: 2026-08-01
Scope: Windows x86 CPU analysis path only. No GPU.

## Verdict

**Mostly yes** — the codebase already approximates the four-layer split. What is missing is mostly **explicit boundaries and structure-layer headroom**, not a total rewrite.

| Layer | Present? | Where | Gap |
|-------|----------|-------|-----|
| Algorithm | Yes | `Filter` / `AxisPlanRequest` / `build_axis_plan` / Float64 LDLT / metric semantics | Do not SIMD this layer |
| Structure | Partial | `AxisPlanCache`, `analyze_batch` workers, `CpuWorkspace` grow-to-fit | Plan reuse across candidates is good; **full-frame intermediates still materialize**; fusion incomplete; layout not SoA across candidates |
| ISA backend | Yes | scalar + SSE2/AVX2/AVX-512 TU + NEON | Production FMA on x86; auto picks AVX2 on this host |
| Microarch | Thin | `worker_count = hardware_concurrency()`, fixed lane widths | No tunable tile/unroll/prefetch policy surface |

---

## 1. Algorithm layer (math semantics)

**Owns:** candidate geometry, taps, banded `AᵀA` / LDLT, forward weights, border mode, metric crop/threshold/p-norm.

| Piece | Location |
|-------|----------|
| `AxisPlan` / `AxisPlanRequest` | `engine/include/getnative/axis_plan.hpp` |
| Plan build | `engine/src/planner/axis_plan.cpp`, `axis_planner.cpp` |
| Filters | `filter.hpp` / planner |
| MetricSpec | `cpu_analysis.hpp` |

**Contract:** Float64 plan construction stays exact; execution consumes frozen Float32 plan tables.
**Phase 2 rule:** do **not** “optimize” by changing planner math.

---

## 2. Structure optimization layer

**Owns:** cache, batching, buffer lifetime, stage fusion, memory layout.

| Capability | Status | Notes |
|------------|--------|-------|
| `AxisPlanCache::get_or_build(_batch)` | **Yes** | Session/warm cache; cold miss rebuilds |
| Batch candidate parallelism | **Yes** | `analyze_batch` → `jthread` pool, one workspace per worker |
| Scratch reuse (`CpuWorkspace`) | **Partial** | Per-worker grow-to-fit; `resize` not free on first sizes; no cross-call session arena on CPU public API |
| Avoid full reconstruction frame | **Partial** | Vertical path fuses last forward into metric via `add_vertical_reconstruction_row`; two-axis path still writes intermediate/native full maps |
| Fuse recon / residual / metric | **Partial** | Vertical fused; horizontal uses `reconstruction_row` then abs-diff; metric still scalar `MetricAccumulator::add` per lane |
| Contiguous vs gather | **Partial** | Column-SIMD walks adjacent RHS (good); transpose/index tables still gather from plan |
| Same plan for specialized/generic | **Yes** | One `AxisPlan`; ISA only changes execution |
| Reduce sync / contention | **OK-ish** | Atomic cursor + exception latch; no heavy locks on hot path |

### Phase 2 focus (generic structure only)

Priority order (no ISA micro-tuning first):

1. **Profile-proven** intermediate traffic (write/read full `intermediate` / `native`).
2. **Session-level** workspace / plan residency for repeated analyze (if product shape needs it).
3. Metric lane accumulation without changing pixel submission order.
4. Optional: better layout for multi-candidate (only if profile shows cache thrash).

**Explicit non-goals for early Phase 2:** hand-unrolled AVX-512 tricks, per-filter SASS-style specials beyond existing B3/B7 fixed half-bandwidth templates, changing candidate order.

---

## 3. ISA backend layer

| Backend | Files |
|---------|-------|
| Dispatch / features | `cpu_features.hpp`, `cpu_features_x86.cpp` |
| Column inverse (P0) | `inverse_columns_*.cpp`, `inverse_columns_x86_impl.hpp` |
| Row recon / abs-diff (P1) | same ISA TUs + hooks in `cpu_analysis.cpp` |
| Scalar / NEON | baseline + `inverse_columns_neon.cpp` |

SIMD lanes already process **independent RHS columns**, not the triangular recurrence along the solve chain — matches the recommended model.

---

## 4. Microarchitecture strategy layer

| Knob | Today |
|------|-------|
| Thread count | `hardware_concurrency()`, capped by candidate count |
| Batch size | All candidates in one `analyze_batch` |
| Tile / prefetch / unroll | Compile-time only inside ISA templates |
| Power / affinity | Not managed in engine |

Keep this layer **data-driven after uProf**. Do not invent knobs without hotspots.

---

## Mapping user Phase 2 checklist → current code

| User item | Already? | Next if profile warrants |
|-----------|----------|---------------------------|
| Avoid re-building AxisPlan | Yes (cache) | Ensure callers use warm cache (bench e2e intentionally cold) |
| Reuse scratch / no hot alloc | Partial | Reserve once to max shape; avoid shrink/grow thrash |
| Reduce intermediate writeback | Partial | Fuse more stages on vertical/both |
| Fuse recon/residual/metric | Partial | Extend vertical fusion; vector-friendly accumulator |
| Contiguous access vs gather | Partial | Pack weights/indices if gather dominates |
| Coarse parallel by candidate | Yes | Affinity only if contention shows |
| SIMD independent RHS | Yes (P0) | Keep; do not vectorize dependency chain |
| Lower branch / indirect | Partial | Fixed B3/B7 paths exist |
| Specialized ≡ generic math plan | Yes | Keep |
| Less sync / cache-line fights | Mostly | Watch false sharing on result writes |

---

## Benchmark vs production structure note

`cpu_backend_benchmark` **end_to_end** rebuilds a fresh `AxisPlanCache` every sample (by design for cold/e2e).
**complete_execution** uses prebuilt plans — that is the structure-layer denominator for SIMD/kernel work.

Phase 2 claims must separate:

- `complete_execution_ms` → execution structure + ISA
- `end_to_end_ms` → planner + cache policy + execution
