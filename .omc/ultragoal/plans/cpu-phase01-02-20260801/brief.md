# CPU Phase 0/1/2 Ultragoal

## Objective
1. Finish Phase 0 multi-ISA baseline (dev-810, 3 cases x 4 ISAs) without parallel heavy load
2. Close Phase 1: production FMA codegen evidence, production tolerance correctness, multi-ISA uplift table
3. Enter Phase 2: structure-layer generic opts only (plan cache reuse, scratch, fusion, layout) guided by uProf; no micro-ISA over-optimization
4. Save typical uProf profiles for primary auto/avx2 path
