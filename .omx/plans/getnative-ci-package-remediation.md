# GetNative-VF CI Slimming and Package Remediation Plan

## Objective

Restore deterministic Linux and Windows release packaging, raise Vulkan validation from compile-only evidence to a software-runtime smoke path, and reduce the default pull-request CI cost without removing cross-platform or release evidence.

This plan is based on the current `codex/ci-package-fixes` branch at `7e755f9`. It preserves the already-completed deletion of the duplicate `App checks` workflow and the existing switch to a prebuilt Windows Vulkan SDK.

## Baseline and Constraints

- The latest package run, `33092831087`, failed in both package jobs. Windows failed while compiling the CUDA fatbin because `crt/host_config.h` was absent; Linux built both bundles and then rejected the AppImage because linuxdeploy-modified `libvulkan.so.1` files no longer matched the source SDK hash.
- Windows currently requests only `nvcc`, `cudart`, `cuobjdump`, and Visual Studio integration in `.github/workflows/package.yml:276-282`.
- Linux currently requires at least one packaged loader to retain the exact source SDK SHA-256 in `.github/workflows/package.yml:192-207`, even though AppImage creation can strip or patch ELF files.
- The engine already compiles and validates embedded SPIR-V and registers a Vulkan runtime test in `engine/CMakeLists.txt:1013-1036`; the latest package run skipped that runtime test because no usable software Vulkan device was configured.
- The current PR surface still runs `ci-fast`, two media jobs, four Linux engine jobs, Metal, and upstream conformance. The measured main run at `7c6410f` used about 33 runner-minutes; the current branch removes roughly 6.5 duplicate runner-minutes but still leaves about 27 runner-minutes for a broad source change.
- Repository `main` currently has no branch protection or ruleset, so check-name changes do not need a simultaneous protection migration. Recheck this immediately before merging.
- Do not weaken release provenance merely to make the AppImage assertion pass. Build/package/runtime evidence must remain separately identifiable.

## Target CI Model

| Layer | Trigger | Required work | Intended cost/latency |
|---|---|---|---|
| PR fast gate | Every relevant pull request | Changed-area detection, frontend tests/build when relevant, Rust test/clippy when relevant, Linux CPU engine tests and Windows CPU portability when engine/build glue changes | Target under 10 runner-minutes and under 7 minutes wall time for a broad PR; much less for frontend-only or docs-only changes |
| PR specialized gate | Only relevant path groups | Linux x64 Vulkan compile/SPIR-V; macOS Metal for engine/Metal changes; media integration only for media/FFmpeg/package-resource changes | No ARM64 or upstream conformance on ordinary PRs |
| Main confidence | Every push to `main` | Linux x64/ARM64 CPU and Vulkan, macOS Metal, upstream conformance, Linux/Windows media integration | Preserve full cross-platform evidence after merge |
| Release rehearsal | `workflow_dispatch` | Full Linux deb/AppImage and Windows portable package, CUDA/Vulkan toolchains, package manifests, extracted-package smoke checks | Expensive by design; no publishing |
| Release publish | `v*` tag | Same package graph as rehearsal, then publish only after both package jobs pass | Fail closed |

## Implementation Steps

### 1. Establish a CI cost and coverage baseline

Files: `.github/workflows/*.yml`, optional new `docs/ci.md` or a concise workflow comment.

1. Record job duration and conclusion for the last three successful or representative runs of each workflow using the Actions API.
2. Classify each job as syntax/unit, compiler portability, backend compile, runtime integration, package integrity, or publication.
3. Record the current baseline: runner-minutes, maximum wall time, cache hit/miss state, and skipped GPU tests.
4. Use the baseline as the comparison point for the final two-run verification; do not claim savings from a single warm-cache run.

Acceptance:

- Every retained job has one explicit evidence category and no two always-on PR jobs have the same platform/backend/test responsibility.
- Cost reporting separates runner-minutes from wall-clock latency.

### 2. Restructure the PR fast gate around changed areas

File: `.github/workflows/ci-fast.yml`.

1. Add a small `changes` job using a commit-SHA-pinned path-filter action, with groups for `frontend`, `rust`, `engine`, `media`, `vulkan`, `metal`, and `ci-package`.
2. Keep stable job/check names and add an always-running aggregate `CI Fast / gate` job so future branch protection can require one check even when component jobs are skipped.
3. Split the current `Frontend and Tauri checks` job:
   - Frontend: Node/pnpm only; run `pnpm test`, `pnpm test:locale`, and `pnpm build`. Do not install WebKit, Rust, or compile the engine.
   - Rust/Tauri: install Tauri system dependencies and run `cargo test` plus clippy only when Rust/Tauri inputs changed.
   - Engine CPU: run Linux CPU configure/build/CTest only when engine or build glue changed.
   - Windows portability: retain the current Windows CPU configure/build/CTest for engine/build-glue changes because it is the low-cost MSVC portability gate (`.github/workflows/ci-fast.yml:16-39`).
4. Probe whether `cargo test` truly needs populated `bundle-stage` resources. If it does, stage a minimal test fixture or retain a CPU stage only in the Rust job; do not compile the engine in the frontend job.
5. Make docs-only changes skip component jobs while the aggregate gate still succeeds.

Acceptance:

- Frontend-only PR: frontend job runs; Rust and engine jobs skip.
- Rust-only PR: Rust job runs; engine/platform jobs run only if shared native inputs changed.
- Engine change: Linux CPU and Windows CPU tests run.
- Docs-only PR: no compiler job is allocated; aggregate gate succeeds.
- Broad PR fast gate uses no more than 10 runner-minutes over two representative runs, unless a documented cold toolchain download accounts for the excess.

### 3. Move expensive platform evidence out of the unconditional PR path

Files: `.github/workflows/engine-linux.yml`, `.github/workflows/engine-macos.yml`.

1. Linux PR behavior:
   - Run x64 Vulkan compile and SPIR-V validation only for Vulkan/shared engine/CMake changes.
   - Do not run ARM64 CPU or ARM64 Vulkan on PRs.
   - Avoid a duplicate x64 CPU job when the fast gate already ran the same CPU configuration.
2. Linux main/manual behavior:
   - Preserve x64 and native ARM64 CPU tests.
   - Preserve x64 and native ARM64 Vulkan compile/SPIR-V validation (`.github/workflows/engine-linux.yml:17-91`).
3. macOS PR behavior:
   - Run Metal only for engine/shared/Metal/CMake changes.
   - Remove the unused CPU-only configure inside the Metal job unless it is upgraded to an actual build/test with a distinct contract; a configure-only directory is not meaningful release evidence (`.github/workflows/engine-macos.yml:42-69`).
4. macOS main/manual behavior:
   - Preserve Metal build/CTest.
   - Run upstream conformance only on `main` and manual dispatch, not every PR (`.github/workflows/engine-macos.yml:71-127`).
5. Use job-level `if` conditions driven by the shared change outputs, or a small reusable change-detection workflow, rather than duplicating inconsistent glob lists.

Acceptance:

- Ordinary engine PR allocates no ARM64 runner and no upstream-conformance runner.
- `main` continues to prove Linux ARM64 CPU/Vulkan and macOS Metal/upstream conformance.
- Vulkan-specific PRs still compile all embedded shaders and run `spirv-val` on Linux x64.

### 4. Restrict media integration to media-relevant PRs

File: `.github/workflows/ci-media.yml`.

1. Keep both Linux and Windows media jobs on every `main` push and manual dispatch.
2. On PRs, run them only when changes touch engine/app media code, FFmpeg build scripts, media tests, runtime staging, Tauri resource mapping, dependency locks, or the media workflow itself.
3. Preserve the current pinned FFmpeg ABI, forbidden CLI checks, dependency closure, and RPATH assertions (`.github/workflows/ci-media.yml:19-112`).
4. Make cache-key OS naming consistent between media and package workflows so compatible SDK builds can be reused only when their build script, FFmpeg version, OS, ABI, and compiler contract match. Do not share caches if those inputs differ.
5. Treat a cache miss as valid but visible; do not retry or silently fall back to an unpinned system SDK.

Acceptance:

- Frontend-only and planner-only PRs allocate no FFmpeg build runners.
- A change to `scripts/build-ffmpeg-*.sh`, media code, staging code, or FFmpeg version runs both OS media jobs.
- Main keeps both media integration jobs regardless of changed paths.

### 5. Repair Windows package toolchain provisioning

File: `.github/workflows/package.yml`.

1. Retain `jakoch/install-vulkan-sdk-action@v1.6.0`; it already bypasses the failing source-built Vulkan-Loader MASM path (`.github/workflows/package.yml:295-303`).
2. Expand Windows CUDA subpackages to the proven compiler set: `nvcc`, `nvvm`, `cuobjdump`, `cudart`, `crt`, `nvtx`, and `visual_studio_integration`.
3. Extend the CUDA preflight to require `include/crt/host_config.h` and the NVVM/libdevice payload.
4. Add a tiny CUDA fatbin compile probe that includes `cuda_runtime.h`. It must run before pnpm/Tauri setup so missing CUDA components fail in seconds.
5. Retain the full application build, ZIP assembly, compiled-backend checks, exact Windows DLL hashes, provenance, licenses, and checksums (`.github/workflows/package.yml:373-497`).

Acceptance:

- The preflight reproduces and prevents the prior missing `crt/host_config.h` failure.
- Windows builds CUDA SM75/86/89/120 plus configured PTX, compiles Vulkan shaders, assembles exactly one portable ZIP, and passes extracted-package verification.

### 6. Correct the Linux AppImage Vulkan provenance contract

Files: `.github/workflows/package.yml`, `app/scripts/build-engine.mjs`, and only if required `app/src-tauri/tauri.conf.json`.

1. Preserve and verify the exact source Vulkan Loader hash before Tauri packaging; keep that value as `source_sha256` in build provenance.
2. First test whether linuxdeploy can be configured to preserve the staged Loader byte-for-byte without compromising bundle creation. If exact preservation works, keep the strict source hash assertion.
3. If linuxdeploy necessarily strips or patches the ELF, stop requiring byte identity after AppImage transformation. Instead:
   - Require the expected loader paths and reject unexpected additional loader locations.
   - Record final packaged hashes in an external artifact manifest generated after deb/AppImage extraction.
   - Verify ELF class, machine, SONAME, absence of unresolved dependencies, and expected Vulkan Loader identity/version.
   - Keep the source SDK hash and final packaged hash as separate provenance fields; never label a transformed hash as the original SDK hash.
4. Continue exact hash validation for bundle formats where the file remains unmodified, such as the deb if current evidence confirms byte preservation.
5. Replace the current “ignore non-pinned system loader” wording with explicit classification of the staged application loader and linuxdeploy dependency copy. Fail on any unclassified third copy.

Acceptance:

- Linux package verification passes for both deb and AppImage without accepting arbitrary Loader binaries.
- A substituted Loader, extra unexpected copy, wrong SONAME/architecture, missing license, or broken dependency closure fails with a specific message.
- Provenance distinguishes source bytes from post-linuxdeploy bytes.

Stop condition:

- If no stable way exists to identify the transformed Loader beyond its filename, do not weaken the hash check. Pause package publication and either prevent linuxdeploy mutation or explicitly repack the trusted Loader after linuxdeploy with a reproducible extraction/rebuild procedure.

### 7. Add software Vulkan runtime evidence to release rehearsal

Files: `.github/workflows/package.yml`, possibly `.github/workflows/engine-linux.yml` for shared setup.

1. Install Lavapipe and Vulkan validation layers in Linux package rehearsal, following the proven dsmvc pattern.
2. Set `VK_DRIVER_FILES` and `VK_LAYER_PATH` explicitly and print `vulkaninfo --summary` so the selected device is auditable.
3. Run CTest with the package configuration and assert that `getnative_vulkan_analysis_tests` passed rather than skipped. The test is already registered in `engine/CMakeLists.txt:1021-1036`.
4. After extracting deb/AppImage, run the packaged engine capabilities check plus the smallest available real Vulkan analysis/workload against Lavapipe. If the CLI lacks a bounded workload entry point, add a narrow smoke command or a package-smoke test executable rather than treating `compiled:true` as runtime proof.
5. Keep CUDA evidence correctly scoped: hosted runners prove fatbin compilation/inventory, not CUDA device execution.

Acceptance:

- Release rehearsal logs name llvmpipe/Lavapipe and show the Vulkan runtime test as passed, not skipped.
- Validation-layer errors fail the job.
- The extracted package, not only the build-tree binary, participates in a Vulkan runtime smoke.

### 8. Harden triggers, action provenance, and release gating

Files: all `.github/workflows/*.yml`.

1. Pin third-party Actions to immutable commit SHAs, retaining version comments for maintainability.
2. Keep `cancel-in-progress: true` for PR/main CI and `false` for tag package runs.
3. Keep package publication fail-closed: `Publish GitHub Release` must continue to need both package jobs (`.github/workflows/package.yml:499-515`).
4. Add `workflow_dispatch` to the slim/specialized workflows where missing so failures can be reproduced without fake source changes.
5. Recheck branch protection/rulesets before renaming checks; if protection is added, migrate it to the aggregate fast gate before deleting old check names.
6. Do not introduce nightly schedules initially. Add a weekly full rehearsal only if main evidence proves insufficient or SDK/toolchain drift is recurring.

Acceptance:

- A failed or cancelled Linux/Windows package job cannot publish a release.
- All action references are immutable except first-party toolchain selectors whose mutability is explicitly justified.
- Manual dispatch can reproduce every specialized lane.

## Verification Sequence

1. Static validation: parse all YAML, run `actionlint`, `git diff --check`, and verify workflow/job dependency graphs.
2. PR routing fixtures: use representative changed-file sets for docs-only, frontend-only, Rust-only, engine-shared, Vulkan, Metal, media, workflow, and package changes; assert the intended job set for each fixture.
3. Targeted CI runs: execute fast, Linux Vulkan, macOS Metal, and media workflows independently.
4. Package rehearsal run 1 with cold or cleared relevant caches to prove provisioning completeness.
5. Package rehearsal run 2 with warm caches to prove cache correctness and reproducibility.
6. Download artifacts using the Actions artifacts API; verify archive counts, SHA256SUMS, required files, provenance, licenses, forbidden developer tools, and extracted engine startup.
7. Compare runner-minutes and wall time against the recorded baseline. Report both warm and cold measurements.
8. Tag publication is allowed only after two consecutive rehearsal runs pass on the same commit and all specialized main checks are green.

## Testable Success Criteria

- Package: Linux and Windows jobs both pass twice consecutively on the same commit; no release is created during manual rehearsal.
- Windows: CUDA compile probe and production fatbin build pass; Vulkan SDK install and shader compilation pass.
- Linux: deb and AppImage verification pass with explicit source/final Loader provenance; no unclassified Loader copy exists.
- Vulkan: SPIR-V validation passes and at least one Lavapipe runtime test executes rather than skips.
- PR cost: broad PR fast/default lanes are at least 50% lower in runner-minutes than the pre-slimming 33-minute baseline; frontend-only and docs-only changes are lower still.
- Main coverage: native Linux ARM64 CPU/Vulkan, macOS Metal, upstream conformance, and both media integrations remain represented.
- No regression in FFmpeg ABI checks, packaged dependency closure, license inventory, backend capability flags, CUDA architecture inventory, or release fail-closed behavior.

## Risks and Mitigations

- Path filters miss a dependency: centralize path groups, add routing fixtures, and treat CMake/build-script/workflow changes as broad native changes.
- Skipped jobs accidentally satisfy protection: require the aggregate fast gate and test its `needs`/result logic for failure, cancellation, and skip states.
- AppImage provenance is weakened: preserve source and transformed identities separately and require runtime/ELF checks; stop rather than accepting filename-only trust.
- Warm cache hides missing SDK components: require one cold-cache rehearsal and explicit file/compile probes.
- Moving ARM64/upstream checks delays feedback until merge: keep them on main/manual, and allow a PR label or manual dispatch for risky platform work.
- Frontend/Rust split breaks Tauri resource discovery: run the no-stage probe first and retain a minimal resource fixture or Rust-only CPU stage if required.

## Recommended Delivery Order

1. Package P0: Windows CUDA component/preflight fix and Linux Loader diagnostic/provenance correction.
2. Package validation: Lavapipe runtime path and two consecutive manual rehearsal runs.
3. CI routing: changed-area fast gate and aggregate check.
4. Specialized workflows: PR/main split for ARM64, Vulkan, Metal, conformance, and media.
5. Supply-chain pins, documentation, duration comparison, and final tag rehearsal.

Do not combine all phases into one unobservable commit. Keep package correctness separate from CI trigger slimming so a red run can be attributed to either build behavior or orchestration changes.
