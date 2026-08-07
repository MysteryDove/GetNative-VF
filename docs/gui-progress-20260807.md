# GUI Progress Record — 2026-08-07

- Status: UI lane complete up to the engine boundary; engine lane paused per
  project decision (see `docs/dsmvc-port-strategy.md`, phase E0 is the
  unblock).
- Baseline: merge `af48396` (project foundation GUI + perf stack).
- Canonical spec: `docs/gui-development-spec.md`; product contract:
  `DESIGN.md`.

## What this session delivered

All work is app-side (`app/src/**`, `app/src-tauri/src/**`); `engine/**` is
untouched. Every runnable control that needs the absent `analyze` command is
capability-gated with an explicit reason; no fabricated results anywhere.

### GUI-4 — Kernel Analysis and Recipe (UI complete)

- `app/src/engine/kernelDraft.ts`: kernel scan draft — preset families
  (deterministic fixed order) and Bicubic Grid mode (decimal b/c sweeps,
  b-outer/c-inner), Lanczos taps, exact ordered candidate sequence.
- `app/src/engine/kernelRunGroup.ts`: one member KernelRun per Sample, one
  fixed geometry per source shape, full ordered candidate list; plan guards
  reuse `validateKernelShape`; `extractKernelResultRows` for real results only.
- `app/src/pages/KernelAnalyzePanel.tsx`: geometry resolved read-only through
  the real engine `geometry` command (`app/src/engine/geometryResolve.ts`),
  grouped by distinct source shape; MetricSpec inherits from Resolution Test
  by default with an explicit unlink path.
- Recipe domain (`app/src/project/recipe.ts`): draft → locked → superseded
  lifecycle, full semantic payload (geometry, kernel, MetricSpec, profile,
  math mode), parent/revision derivation, atomic single-pointer activation,
  guarded removal. `recipeApply.ts` applies analysis payloads to the current
  draft from both analyze tabs.
- `RecipeReviewDialog` + `RecipeManager` on the Overview: full semantic
  summary before lock, lock never silently activates, derive-new-revision,
  single Active badge.
- Manifest schema stays at 1: new Recipe fields are optional and round-trip
  through `manifest.rs`; legacy `locked` manifests migrate (`locked=true` →
  status locked).

### GUI-5 — Verify setup and Results (UI complete, execution gated)

- `app/src/engine/verifyPlan.ts` + `app/src/pages/VerifyPage.tsx`: multi-source
  selection, visually distinct Full Verification vs Preview Scan
  (decoded-I-picture / every-N), exact frame range, backend preference,
  read-only active-Recipe strip; per-source member VerificationRun planning
  validated by `validateVerifyShape`.
- `app/src/pages/ResultsPage.tsx`: RunGroup history with expandable members,
  MetricSpec-compatibility-gated comparison (best-point table from real
  stored series), JSON export with full provenance and CSV export of raw
  numerics via the new app-side `export_artifact` command
  (`app/src-tauri/src/export.rs`).

### GUI-2 — acceptance evidence added

- `media.rs`: pure `probe_video_value` fixture matrix — multi-stream
  filtering/selection, VFR with undeclared frame count, audio-only
  container → explicit unsupported state; missing path and unknown container
  are actionable errors; irregular-VFR timestamp → frame round-trip test.
- Pre-existing evidence confirmed: keyboard frame stepping and duplicate
  detection (`frameBrowser.test.ts`), exact frame/time round-trip and
  bounded preview cache (`media.rs` tests), staged-FFmpeg VFR smoke test
  (ignored by default; needs `GETNATIVE_MEDIA_SMOKE_FIXTURE`).

## Verification

- `npm run build` clean; `vitest` 28 passed / 7 files; locale parity 447 keys
  (`node src/i18n/check-locale.mjs`).
- `cargo test --lib` 39 passed; `cargo clippy --all-targets -- -D warnings`
  clean.

## Boundaries (recorded, not simulated)

- Engine CLI exposes `capabilities` + `geometry` only
  (`engine/src/cli/main.cpp:280-289`, `"analyze": false`). GUI-3/4/5 run
  execution, live progress, and real result ingestion wait for the E0
  resident-worker protocol (`docs/dsmvc-port-strategy.md` §6).
- Frontend protocol types for height/kernel/verify requests and worker events
  already exist (`app/src/engine/protocol.ts`); RunGroup materialization is
  implemented and tested against them.
- Screenshot evidence (both locales, 1440x900 and ~1024x768) is **not**
  automated in the current environment (no Wayland screenshot permission).
  Manual capture pending; layout uses stable panes and bounded lists per the
  spec's layout rules.

## Suggested next step

When the engine lane lands E0 (`commands.analyze=true` over the JSONL worker
protocol), the remaining UI work is wiring only: spawn the worker from
`engine.rs`, stream events into `runReducer`, and enable the already-planned
Run start buttons.
