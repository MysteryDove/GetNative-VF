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

## Update — GUI-3 wired end-to-end (same day, later session)

The E0 Tauri transport lane (`app/worker-transport`) is merged into `main`.
The engine-source merge is intentionally deferred: an E1 agent is actively
working on `engine/worker-protocol` in its own worktree; the app codes
against worker protocol v1 and is forward-compatible with E1's CUDA work.

- `app/src/engine/executeRunGroup.ts`: RunGroup orchestration — materialize
  immutable queued Runs, per-member frame-asset export (`media_frame_asset`),
  `submitHeight` with wire-mapped kernel/metric/grid, member failures marked
  failed without aborting the group, live jobs bound to persistent Run ids.
- `App.tsx`: terminal worker events (result/cancelled/error) bridge onto
  Project Runs append-only; progress stays in the transient execution state
  (Job Tray) and never churns autosave.
- `AnalyzePage`: the Run button is real when the worker reports
  `commands.analyze=true`; Job Tray offers member-level cooperative cancel.
- `extractHeightSeries` reads the worker v1 payload
  (`{candidates: [{id, error}]}`), so real engine curves land on the plot and
  table with zero shape translation.
- Frontend default metric aligned to p-norm 1 (getnative default; the v1 CPU
  backend's supported norm).
- Kernel/Verify pages stay gated with the accurate reason: engine worker v1
  implements height mode only.

Evidence: vitest 39 passed (incl. orchestration tests with a fake worker and
append-only terminal-event tests); `cargo test --lib` 48 passed;
`GETNATIVE_ENGINE_PATH=<worker build> cargo test worker:: -- --ignored`
2 passed (real engine roundtrip, session plan-cache hits, frame-asset →
analyze loop); clippy clean; locale parity 454 keys; `npm run build` clean.
GUI click-through on video Samples still needs the staged-FFmpeg host
(recorded boundary).

## Update 2 — QoL batch (2026-08-08, commit cf3c822)

- Samples page accepts file drops: still images become included Samples
  directly (dedup-safe), videos import as Sources with a Media-page hint.
  The import pipeline is now shared (`media/importSources.ts` +
  `useFileDrop`); Media page picker/drop behavior unchanged.
- Height plot reading aids: valley (best point) highlight, perfectly-descale
  1e-6 threshold line (raw and log display), perfect-point styling in the
  data table — the training-workflow reading actions are now first-class UI.
- Base-canvas overrides (baseHeight/baseWidth = the parity exploration
  contract) flow into every height request and intent snapshot with decimal
  validation; engine v1 ignores them safely, v1.1 consumes them.
- Training-coverage assessment (总监培训 day1/day2) drove these; remaining
  gaps: verify review loop (timeline/threshold/Top-N/add-to-Samples) awaits
  the engine verify mode; kernel mode likewise.

## Suggested next step

GUI-3's remaining acceptance item is a manual click-through smoke (still
Sample → Resolution Test → live curve → persisted Run → Results export) plus
the staged-FFmpeg media smoke on a host with the sidecars. When the engine
lane lands E1 (CUDA batch scheduler) and extends worker modes, the Kernel
and Verify pages need only their submit wiring — planning, guards, and
payload extraction are already implemented and tested.
