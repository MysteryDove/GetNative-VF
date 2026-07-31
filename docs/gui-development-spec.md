# GUI Development Specification

## Status and precedence

- Status: Ready for staged implementation planning.
- Date: 2026-07-31.
- Canonical product and UX contract: `DESIGN.md`.
- This document translates `DESIGN.md` into development slices and acceptance criteria. If the two documents conflict, `DESIGN.md` wins.
- UI implementation must use a dedicated Tauri/UI branch and worktree from the agreed integration baseline. It must not enter a performance worktree or take ownership of `engine/**`.

## Target outcome

Replace the geometry-only prototype with a Project-based desktop workflow that can progressively attach to real engine capabilities without showing fabricated analysis results.

The intended user journey is:

1. Open or create a Project.
2. Import still images and videos.
3. Select representative stills and video frames as Samples.
4. Run integer Height Analysis and optionally refine with fractional geometry.
5. Run Kernel Analysis at the selected geometry.
6. review, name, lock, and activate one Analysis Recipe.
7. Run Full Verification or a clearly incomplete Preview Scan on one or more videos.
8. Review high-error frames, add them to Samples, and repeat analysis when needed.
9. Reopen immutable Runs and export structured evidence.

## Current baseline

Observed current behavior:

- React renders one dense geometry workbench in `app/src/App.tsx`.
- Rust exposes only `engine_capabilities` and `engine_geometry`.
- The engine capability contract reports analysis availability separately.
- Media probe/decode, Project persistence, worker events, real analysis, verification, and export are not integrated with the GUI.
- The prototype correctly avoids fake curves and disables analysis when the command is unavailable. Preserve that rule.

Consequences for frontend development:

- Pages may be implemented before their engine command exists, but runnable controls stay capability-gated.
- Component fixtures may exist in tests or isolated development harnesses; they cannot appear as real Project results.
- Existing geometry preview remains available as a diagnostic/geometry sub-surface until real Height Analysis replaces it.
- Full-resolution decoded frames must use an engine-owned bounded asset/cache path, not JSON base64 payloads.

## Language and terminology contract

- Supported locales are Simplified Chinese (`zh-CN`) and English (`en`). First launch defaults to `zh-CN`; a persisted application preference takes precedence afterward.
- Do not coin UI terminology. For concepts covered by `总监培训2026_20260725.html` or `总监培训2026_20260726.html`, the approved UI wording must prefer terms already present in those sources.
- Preserve canonical technical spellings such as `getnative`, `descale`, `Bicubic`, `Bilinear`, `Spline`, and `Lanczos` in both locales.
- English route, page, and domain names in this specification are architectural identifiers. They are not permission to hard-code English display text.
- Maintain one reviewed bilingual terminology mapping for navigation, commands, parameters, statuses, errors, and accessibility labels. When a required concept is absent from both source HTML files, use established industry terminology and document the mapping; never fabricate a new word.
- Store complete phrases per locale. Do not concatenate translated fragments, and do not mix locales except for canonical technical names, formats, code values, and user content.
- Locale selection is application state, not Project state. Project manifests, Recipes, Runs, metrics, and exports remain language-neutral.
- Rust and engine boundaries return stable codes and structured values; React localizes user-facing messages. Raw diagnostic text may be shown separately and clearly labeled.

## Application composition

```text
+--------------------------------------------------------------------------------+
| Project name | save state | active jobs | engine/backend | contextual actions |
+----------+---------------------------------------------------------------------+
| Overview | page toolbar / prerequisite strip                                   |
| Media    +------------------+--------------------------------+-----------------+
| Samples  | source/sample    | primary evidence               | inspector       |
| Analyze  | list             | frame / plot / timeline        | parameters      |
| Verify   |                  |                                | results/review  |
| Results  +------------------+--------------------------------+-----------------+
|          | persistent expandable global job tray                               |
+----------+---------------------------------------------------------------------+
```

Layout rules:

- The center evidence surface owns remaining space and never sits inside a decorative card.
- Left and right panes have stable min/max widths and collapse into drawers at compact desktop widths.
- Tables, plots, frame viewports, and timelines have stable dimensions while data streams.
- Page sections use borders and surface changes; do not nest cards.
- The active Project, Source/Sample identity, Recipe, capability state, and Run coverage remain visible at the point of action.

## Route and responsibility matrix

| Route | Owns | Must not own | Completion signal |
| --- | --- | --- | --- |
| `/projects` | Project creation/open/recovery and recent list | Analysis parameters | A validated Project is open |
| `/project/:id/overview` | Readiness, blockers, Recipe activation, recent Runs | Detailed analysis controls | User sees the correct next action |
| `/project/:id/media` | Import, probe, source inspection, exact frame selection | Candidate grids | Ready Sources and Sample references |
| `/project/:id/samples` | Included analysis set, tags, ordering, validation | Source deletion or analysis execution | Ordered valid Sample set |
| `/project/:id/analyze/height` | Candidate heights, axes, fixed kernel, MetricSpec, Height Runs | Multiple kernels in one engine Run | Selected geometry draft |
| `/project/:id/analyze/kernel` | Fixed geometry, kernel candidates, inherited MetricSpec, Kernel Runs | Height mutation | Selected kernel draft |
| `/project/:id/verify` | Source batch, scan scope, coverage, execution, frame review | Recipe semantic editing | VerificationRun results and review annotations |
| `/project/:id/results` | Immutable Run/RunGroup history, comparison, provenance, export | Editing historical inputs | Reproducible artifact or comparison |

## Core frontend state

Use normalized Project state rather than page-local copies of domain objects:

```text
ProjectState
  project
  sourcesById
  samplesById
  recipesById
  runGroupsById
  runsById
  verificationReviewsByRunId
  jobsById
  uiStateByRoute
```

State rules:

- Page forms edit drafts. Starting a Run creates an immutable input snapshot.
- Streamed progress updates Job and Run execution state, not the form draft.
- Completed result objects are append-only.
- `active_recipe_id` is the only activation pointer. Activation replacement is atomic.
- VerificationReview is mutable and separate from immutable VerificationRun metrics.
- Project autosave records domain mutations, not transient hover, plot cursor, or scrubbing state.
- Source relinking preserves Source ID only after fingerprint revalidation is explicit.
- The current locale lives in persisted application preferences outside `ProjectState`; changing it re-renders the UI without mutating Project domain objects.

## UI to engine boundary

Exact protocol field names remain owned by the engine/protocol integration lane. The GUI requires these semantic capabilities:

| Capability | UI request | UI-visible result |
| --- | --- | --- |
| Capabilities | None | Commands, profiles, kernels, backends, devices, limits, reasons |
| Project file operations | Create/open/save/recover/relink | Validated manifest or actionable error |
| Probe | One or more local paths | Source kind, streams, dimensions, frame/time metadata, color/decode provenance |
| Decode frame | Source, stream, frame or timestamp, preview size | Exact identity plus bounded image asset |
| Thumbnail/filmstrip | Source, stream, requested range/count | Bounded cached image assets with frame identity |
| Analyze | One valid Height or Kernel computation shape | Accepted/progress/warning/result/cancelled/error events |
| Verify | One Source, one locked Recipe snapshot, one ScanScope | Per-frame metrics, progress, coverage, resumable state |
| Export | Run/RunGroup ID and format | Artifact identity and destination |

Boundary requirements:

- Every request has a request/job ID and schema/protocol version.
- Capability absence disables the command and provides a reason; it does not remove existing results.
- Backend or decoder fallback requires a warning event and distinct provenance.
- Cancellation is cooperative and preserves completed partial results where supported.
- Bulk pixel/frame data never crosses JSON.
- The React layer never receives general shell/process access.

## Computation shape guards

Validate these in the form/view-model before invoking Rust, and validate again at the Rust/engine boundary:

| Mode | Sample/frame shape | Geometry/kernel shape | Invalid examples |
| --- | --- | --- | --- |
| Height | One Sample per member Run | One fixed kernel, many heights | Many heights and many kernels in one Run |
| Kernel | One Sample per member Run | One fixed geometry, many kernels | Many heights and many kernels in one Run |
| Verify | Multiple frames from one Source | One fixed locked Recipe | Multiple Recipes in one VerificationRun |

User commands may create RunGroups:

- Multi-Sample Height: one HeightRun per Sample.
- Compare Common Kernels during Height: one HeightRun per Sample/kernel pair.
- Multi-Sample Kernel: one KernelRun per Sample.
- Multi-Source Verification: one VerificationRun per Source.

The Job Tray presents aggregate progress but preserves member-level status and cancellation.

## Page specifications

### Project Hub

Primary controls:

- New Project.
- Open Project.
- Quick Analysis as an untitled recoverable Project.
- Recent Project row actions: open, reveal, remove from recents.

Acceptance:

- First launch enters Project Hub.
- Invalid or newer-schema Projects are not overwritten.
- Missing media does not block Project opening.
- Remove from recents never deletes Project data.

### Project Overview

Primary surface:

- Linear readiness strip: Media, Samples, Height, Kernel, Active Recipe, Verify.
- Compact blockers list with one direct action per blocker.
- Recipe table with exactly one possible Active badge.
- Recent RunGroup/Run table.

Acceptance:

- The primary action is the first unmet prerequisite.
- Activating a Recipe replaces the prior active pointer atomically.
- Historical Runs retain their original Recipe IDs.
- Deleting/trashing the active Recipe is blocked until deactivation or replacement.

### Media and frame browser

```text
+----------------+--------------------------------------+----------------------+
| Sources        | decoded image/frame                  | selected Samples     |
| probe state    | zoom/pan/pixel                       | current source       |
| dimensions     | timeline + filmstrip                 | add/remove actions   |
| stream         | frame step + frame/time input        | tags                 |
+----------------+--------------------------------------+----------------------+
```

Interaction contract:

- Multi-select file picker and drag/drop accept stills and videos together.
- Still images show no frame count, timeline, or timestamp controls.
- Video scrubbing may show a nearby preview while dragging; release resolves the exact frame.
- Previous/next frame and previous/next indexed I-picture/keyframe are separate commands.
- Adding a video frame stores Source, stream, frame index, PTS, and timebase.
- The browser does not imply real-time playback and has no audio controls.
- Credits, letterbox, noisy, bright, and high-detail may be user tags; none are silently excluded.

Acceptance:

- Adding the current frame never requires exporting a PNG.
- Re-selecting an existing frame does not create an indistinguishable duplicate without confirmation.
- Decode errors preserve the Source row and identify the failing stream/operation.
- Frame step controls remain keyboard accessible and selection does not move layout.

### Samples

Primary controls:

- Include/exclude, remove reference, tag, reorder, group by Source.
- Preview selected Sample.
- Start Height Analysis with the included set.

Acceptance:

- Still rows omit frame/time columns.
- Removing a Sample does not delete its Source or historical Runs.
- Stale fingerprints block new Runs while preserving prior results.
- Aggregation is off by default until the product aggregation rule is decided.

### Height Analysis

```text
+----------------+--------------------------------------+----------------------+
| Samples/series | height curves + selected point       | Search preset        |
| visibility     | compatible overlays                  | axis / fixed kernel  |
| colors/status  | equivalent data table                | grid / MetricSpec    |
|                |                                      | work estimate / Run  |
+----------------+--------------------------------------+----------------------+
```

Default flow:

1. Integer Coarse preset.
2. Select one valley/height.
3. Optional Refine Around Selection creates a fractional CandidateGridSpec.
4. Apply selected geometry to Recipe Draft.

Acceptance:

- The UI shows resolved candidate count and work estimate before starting.
- One member HeightRun contains one Sample, one kernel, and many heights.
- Compare Common Kernels is visibly a RunGroup, not a single engine Run.
- Standard integer width is derived from probed aspect ratio.
- W-only requires no transpose action from the user.
- Candidate decimal values and endpoint semantics are inspectable.
- Changing MetricSpec separates incompatible results instead of overlaying them.

### Kernel Analysis and Recipe review

Default flow:

1. Select compatible Height Draft/result.
2. Compare preset families.
3. When Bicubic is plausible, switch to Bicubic Grid and inspect `b/c` heatmap.
4. Apply selected kernel to Recipe Draft.
5. Open Recipe Review, name and lock the Recipe, then optionally activate it.

Acceptance:

- Geometry is read-only on Kernel Analysis.
- Each member KernelRun contains one Sample, one geometry, and many kernels.
- Kernel candidate count and exact ordered sequence are shown before execution.
- MetricSpec inherits from Height by default.
- A mismatched Height/Kernel MetricSpec blocks locking until explicitly resolved.
- Locking never silently activates or mutates another Recipe.

### Whole-video Verification

```text
+-------------------------------------------------------------------------------+
| Sources | Full / Preview | range | backend | active Recipe | Start            |
+-----------------------------------------------+-------------------------------+
| frame preview                                 | review table                  |
| metric timeline + coverage/progress           | threshold / Top N / tags      |
| log/linear, zoom, selected frame               | reviewed / add to Samples     |
+-----------------------------------------------+-------------------------------+
```

Scan setup:

- Sources: one or more ready videos.
- Full Verification: every eligible frame in the resolved range.
- Preview Scan: decoded I-picture only or every N frames.
- Range: full Source or exact frame/time in/out.
- Execution: backend/device preference; Recipe math mode remains read-only.

Review behavior:

- Pixel Exclusion Threshold is shown in Recipe summary and cannot be edited here.
- Frame Review Threshold and Top N filter may be changed without rerunning.
- Selected timeline point and review-table row remain synchronized.
- Add to Samples creates a linked Sample and keeps the VerificationRun origin.
- Re-analyze opens Height or Kernel with that Sample selected.

Acceptance:

- Multi-source Start creates one member VerificationRun per Source.
- Full and Preview labels cannot be visually confused.
- Preview/partial results show scanned frames / eligible frames.
- Raw zero remains zero in stored/exported data; log epsilon is display-only.
- Cancelled runs keep completed metrics and explicit partial coverage.
- Resume rejects any mismatch in Source, stream, scope, selection rule, Recipe, decode policy, or engine compatibility.

### Results and diagnostics

Results acceptance:

- RunGroup rows expand to member Runs.
- Comparison controls include only semantically compatible results by default.
- JSON exports complete structured provenance; CSV/TXT exports raw numeric data; plot exports disclose coverage and display transform.
- Deleting an artifact or hiding a Run never rewrites provenance links.

Diagnostics acceptance:

- Show engine path/version, commands, profile/kernel/backend limits, media backend, decoder state, cache, and fallback reasons.
- Clear Cache cannot remove Project manifests, Recipes, Runs, or structured results.
- Diagnostic controls do not dominate normal analysis pages.

## Capability and empty-state matrix

| State | Visible behavior | Primary action |
| --- | --- | --- |
| Engine missing | Project data remains accessible; compute actions disabled | Locate/reinstall engine or open diagnostics |
| Command unavailable | Page shell and prior results remain; no runnable fake controls | View capability reason |
| Empty Project | Only missing prerequisite and direct command | Import media |
| Probing/decoding | Source stays selected with bounded progress | Cancel/retry where supported |
| Ready, no Samples | Media remains inspectable | Add current image/frame |
| Running RunGroup | Prior results remain; member progress visible | Inspect/cancel member or group |
| Partial/cancelled | Completed data and coverage remain visible | Resume when compatible or create new Run |
| Stale Source | Prior results remain readable | Relink/revalidate Source |

## Recommended delivery slices

### GUI-1: Project foundation

Ownership:

- `app/src/**`
- `app/src-tauri/src/**` for Project filesystem commands only
- UI-specific tests and fixtures
- No `engine/**`

Deliver:

- Project Hub and ProjectShell navigation.
- `zh-CN` and `en` locale resources, first-launch `zh-CN`, a persisted language selector, and the reviewed bilingual terminology mapping.
- Versioned Project manifest create/open/save/autosave/recovery.
- Normalized frontend store and route view state.
- Existing capabilities and geometry surface moved behind the new shell without changing engine behavior.
- Global engine state and empty/error/capability gating.

Exit evidence:

- Fresh launch, recent Project, recovery, invalid manifest, missing Source, and read-only-newer-schema scenarios.
- First-launch locale, live language switching, preference persistence, locale-key parity, and no untranslated/mixed-language labels.
- `npm run build`.
- Rust unit tests for manifest validation and safe paths.
- `cargo test` and `cargo clippy --all-targets -- -D warnings`.
- Screenshots in both `zh-CN` and `en` at 1440x900 and approximately 1024x768 with no overlap or clipped labels.

### GUI-2: Media and Samples

Dependency: real probe/decode/frame-asset capability.

Deliver:

- Mixed import queue, Source list, still inspector, silent frame browser, Samples table, relink flow.
- Exact frame identity and thumbnail/frame cache integration.

Exit evidence:

- Still-only, video-only, mixed, multi-stream, missing, unsupported, VFR, and unknown-frame-count fixtures.
- Keyboard frame stepping and exact frame/time round-trip checks.

### GUI-3: Height end-to-end

Dependency: real Height analyze command/events.

Deliver:

- Integer/Refine/Custom grids, axes, fixed kernel, MetricSpec, RunGroup orchestration, live plot/table, geometry draft.

Exit evidence:

- Engine-shape guards, exact candidate sequence, cancellation, incompatible MetricSpec, multi-Sample member failures, and immutable reruns.

### GUI-4: Kernel and Recipe

Dependency: real Kernel analyze command/events.

Deliver:

- Preset comparison, Bicubic heatmap, inherited MetricSpec, Recipe review/lock/version/activation.

Exit evidence:

- Exact ordered kernel candidates, `b/c` grid endpoints, Recipe immutability, single activation, and historical ID stability.

### GUI-5: Verification, review, and Results

Dependency: media indexing plus resumable multi-frame verification and export.

Deliver:

- Multi-source RunGroup, Full/Preview scopes, timeline, review rules, anomaly-to-Sample loop, comparison, export.

Exit evidence:

- All-frame, decoded-I-picture, every-N, custom range, partial, cancel, resume, mixed success batch, raw-zero/log-display, and export provenance scenarios.

## Cross-cutting definition of done

A GUI slice is complete only when:

- It uses real typed domain state and capability gates.
- All user-facing copy and accessibility labels exist in both `zh-CN` and `en`; components do not hard-code visible language text.
- UI terminology follows the reviewed bilingual mapping and does not introduce coined labels, invented abbreviations, or fabricated translations.
- Both locales pass compact and wide layout checks without overlap, clipped controls, or incoherent wrapping.
- No placeholder result can be mistaken for engine output.
- Loading, empty, error, disabled, partial, and success states are implemented.
- Keyboard focus remains stable during streamed updates.
- Plot/timeline content has an equivalent accessible table.
- Long lists are bounded or virtualized before production-sized fixtures are accepted.
- Backend/decode fallback is visible and recorded.
- User changes cannot mutate historical Runs or locked Recipes.
- Relevant TypeScript build, Rust tests/clippy, focused interaction checks, and desktop screenshots pass.
- Any engine/protocol dependency not yet available is recorded as a boundary, not simulated as completion.

## Deferred decisions

- Default multi-Sample aggregation rule.
- Visible Project bundle versus single-file package abstraction.
- Required still-image formats beyond PNG/JPEG/TIFF.
- Whether Top Percent joins absolute threshold and Top N in the first review UI.
- Proven decoder mapping for the legacy decoded-I-picture selection rule.
- Frontend component/e2e test dependency selection; adding packages requires a separate dependency/license decision.
