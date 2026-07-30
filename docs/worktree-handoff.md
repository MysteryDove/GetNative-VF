# Worktree Freeze and Performance Handoff

## Status

- Frozen source baseline: `c320503d0b1f2356ada044f87467cc17665fd4fd`
  (`chore: freeze standalone engine baseline`).
- Handoff date: 2026-07-31.
- This handoff separates the staged host/planner work, backend hot-path work,
  and final integration. No feature development happens on `main` after the
  freeze.
- The session `019fb32e-1463-77b1-b402-088590c6bef9` is the Host/Pre-GPU
  planning lane, not the Tauri/React UI lane. It must continue on
  `perf/pre-gpu-stage1`.

## Worktree Map

| Purpose | Branch | Worktree | Authoritative plan |
| --- | --- | --- | --- |
| Stage 0 measurement and conditional Stage 1 host planner work | `perf/pre-gpu-stage1` | `/Users/owen/Documents/GetNative-VF-pre-gpu-stage1` | `.omx/plans/prd-pre-gpu-foundation-staged.md` and `.omx/plans/test-spec-pre-gpu-foundation-stage-1.md` |
| Metal kernels/runtime and ARM64 CPU hot paths | `perf/backend-hotpath` | `/Users/owen/Documents/GetNative-VF-backend-hotpath` | `.omx/plans/metal-kernel-hotpath-design.md` |
| Ordered merge and combined validation only | `integration/perf-stack` | `/Users/owen/Documents/GetNative-VF-perf-integration` | This handoff plus the terminal evidence from both performance branches |
| Frozen reference and future branch point | `main` | `/Users/owen/Documents/GetNative-VF` | Frozen committed project state; no ongoing feature ownership |

All listed worktrees originate from the frozen baseline. A lane may update only
its own branch. Do not continue feature work in the `main` checkout.

## Host/Pre-GPU Lane

Use `perf/pre-gpu-stage1` for the current Host/Pre-GPU agent.

Owned scope:

- Stage 0 planner and phase-separated total timing.
- Immutable JSON benchmark evidence and the Stage 0 decision.
- Conditional Stage 1 unique-plan batch construction.
- Private exact-key extraction from `axis_plan.cpp` when Stage 1 is authorized.
- Planner tests, focused TSan coverage, existing benchmark wiring, and the
  directly required `engine/CMakeLists.txt` changes.

Do not modify:

- `engine/src/backend/metal/getnative.metal`.
- `engine/src/backend/metal/metal_backend.mm`.
- ARM64 NEON execution kernels.
- Metal arenas, upload rings, or packed-plan caches.
- Tauri/React UI files or UI-facing protocols.

The expected handoff revisions are:

- `.omx/plans/prd-pre-gpu-foundation-staged.md`:
  `c90df0aa9a0be50c30f867decff06c48151721708cf606cf63d32b7292be31f2`.
- `.omx/plans/test-spec-pre-gpu-foundation-stage-1.md`:
  `5a5c6972068982b65683d986ccdfdcfe220093afeae7004d5dcfbb7299e1bd68`.

Stage 0 is mandatory. Stage 1 starts only when the valid 21-sample result is
exactly `PROCEED_STAGE1`, defined by
`median(plan_ms_i / metal_total_ms_i) >= 0.10`. A lower valid result records
`STOP_AND_REDIRECT_GPU`; invalid evidence records `STAGE0_BLOCKED` and permits
only repair of the measurement defect.

## Backend Hot-Path Lane

Use `perf/backend-hotpath` for backend-only performance implementation.

Owned scope:

- Fixed-shape Metal B11/B15 kernels.
- Lanczos5-8 Metal function-constant specializations.
- ARM64 NEON adjacent-column SIMD, while preserving a separate portable/x86
  dispatch path.
- Persistent Metal arena, a measured 2- or 4-slot upload ring, and packed-plan
  cache work.
- PL0 tap-weight reuse and PL1 bicubic topology sharing only after the staged
  lane reaches its terminal Stage 1 disposition.

The authoritative plan is
`.omx/plans/metal-kernel-hotpath-design.md`, frozen SHA-256
`2a4524f4a441212df5da3a0ba58922dd7d60f2293112b20bc13facca50d5e5a8`.

Do not modify the staged PRD/test specification or implement Stage 0/1 planner
work here. Until Stage 1 is integrated, do not take ownership of
`engine/src/planner/axis_plan.cpp` or planner benchmark wiring. This lane is
backend-only and does not touch Tauri/React.

## Integration Lane

`integration/perf-stack` is not a development branch. Use this order:

1. Merge the terminal commit from `perf/pre-gpu-stage1` first.
2. Re-run Stage 0 and, when applicable, the Stage 1 gate.
3. Merge the terminal commit from `perf/backend-hotpath` second.
4. Resolve only integration conflicts, primarily CMake and benchmark wiring.
5. Run the complete combined correctness, conformance, race, benchmark, and
   Metal validation set.

Never resolve a behavioral conflict by silently choosing one branch. Return an
ownership conflict to the branch that owns the affected contract, then merge a
new terminal commit.

## Tauri UI Boundary

None of the three performance worktrees owns Tauri/React UI work. If "frontend"
means the actual UI under `app/`, create a separate `feat/tauri-*` branch and
worktree from the frozen baseline. Do not place UI work on
`perf/pre-gpu-stage1`, `perf/backend-hotpath`, or `integration/perf-stack`.

The current `019fb32e-1463-77b1-b402-088590c6bef9` agent is specifically the
Host/Pre-GPU lane and therefore uses `perf/pre-gpu-stage1`.

## Frozen Inputs

The local benchmark image is present in every performance worktree but remains
untracked because redistribution rights have not been established:

```text
engine/bench/fixtures/6.2-1.png
SHA-256 61f9ee1ac858bbadd6a959ba35f5eceb077b8452b91e97a5ce3d39ebc69e20c6
```

Commit only its manifest and checksum. `.gitignore` must continue to exclude the
PNG while retaining `engine/bench/fixtures/6.2-1.png.sha256` and the fixture
README.

Frozen local upstream references:

| Checkout | Commit |
| --- | --- |
| `upstream/GetFnative` | `44c8d0f1ea2ce0c5087d55319c668a86ddb7258d` |
| `upstream/descale` | `8c53f5d1297dee286e5a854ae5731103614a0583` |
| `upstream/muvsfunc` | `d278cd3a68250a4d9562c6ec2b401f1a76c324a3` |
| `upstream/zimg` | `1ad1895d5ff0bbe69c61243f9996aede713d1b5f` |

The `/upstream/` directory is local reference material and remains ignored for
an eventual public repository.

## Direct Handoff To The Current Agent

Use this instruction for the current Host/Pre-GPU agent:

```text
Stop writing in /Users/owen/Documents/GetNative-VF on main.
Continue in /Users/owen/Documents/GetNative-VF-pre-gpu-stage1 on branch
perf/pre-gpu-stage1. Treat
.omx/plans/prd-pre-gpu-foundation-staged.md and
.omx/plans/test-spec-pre-gpu-foundation-stage-1.md as your authoritative plan
and test contract. Preserve their handoff revisions before editing. Complete
and commit the planning artifacts in this branch, then implement and record
Stage 0. Implement Stage 1 only if Stage 0 records PROCEED_STAGE1 with median
plan_ms / metal_total_ms >= 0.10. Do not touch Metal kernels/runtime, CPU NEON,
Metal arena/cache work, Tauri UI, main, or the integration branch. End with one
terminal branch commit plus exact test and benchmark evidence for integration.
```

Before doing any work, the agent must verify:

```sh
cd /Users/owen/Documents/GetNative-VF-pre-gpu-stage1
git branch --show-current
shasum -a 256 \
  .omx/plans/prd-pre-gpu-foundation-staged.md \
  .omx/plans/test-spec-pre-gpu-foundation-stage-1.md
shasum -a 256 -c engine/bench/fixtures/6.2-1.png.sha256
```

The branch command must print `perf/pre-gpu-stage1`, and the plan/fixture hashes
must match this handoff before implementation begins.
