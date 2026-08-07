# Tauri Worker Transport (E0 App-Side Half)

- Status: Implemented; verified against a real worker build.
- Date: 2026-08-07.
- Roadmap anchor: `docs/dsmvc-port-strategy.md` phase E0; engine-side half
  lives on branch `engine/worker-protocol` (`docs/worker-protocol-v1.md`
  there is the authoritative wire reference).
- Lane: `app/worker-transport` (app-side only; no `engine/**` ownership).

## 1. What landed

| Piece | Location | Role |
| --- | --- | --- |
| Worker session manager | `app/src-tauri/src/worker.rs` | Spawns `getnative-engine worker`, JSONL on stdin/stdout, stderr tail capture, hello handshake with timeout, pending-request routing for request/response commands, Tauri event forwarding for job events, dead-worker detection, kill-on-drop |
| Tauri commands | `worker.rs` + `lib.rs` | `engine_worker_start`, `engine_worker_capabilities`, `engine_worker_analyze`, `engine_worker_cancel`, `engine_worker_shutdown` |
| Frame asset export | `app/src-tauri/src/media.rs` (`media_frame_asset`) | Still: image-rs luma f32 (0..=1). Video: FFmpeg `grayf32le` rawvideo pipe (full-range float luma), same `select=eq(n,N)` exact-frame pattern as the shipped preview path. Atomic tmp+rename writes, byte-count validation, bounded LRU cache (`frame-assets/`, 64 files / 512 MiB), fingerprint-pinned cache keys |
| TS transport client | `app/src/engine/workerClient.ts` | `EngineWorkerClient`: idempotent `connect()`, wire(snake_case)→semantic(camelCase) event translation onto `protocol.ts` types, GUI-vs-engine job-id correlation, deferred cancel before `accepted`, unfinished-job failure synthesis on worker exit |
| Media service | `app/src/media/service.ts` (`exportFrameAsset`) | Typed invoke wrapper for the frame asset command |
| App wiring | `app/src/App.tsx` | Worker-first engine init (worker capabilities report `commands.analyze=true` per the backend contract), one-shot CLI fallback, worker events reduced through `runReducer.reduceWorkerEvent` into the existing Job Tray, exit fallback restores one-shot gating |

Boundary rules kept: bulk pixels never cross JSON (frames are file assets);
the GUI never fabricates results (`result` payloads pass through untouched);
capability absence disables rather than simulates; capability validation in
`engine.rs::validate_capabilities` is reused unchanged for the worker
envelope.

## 2. Identity and cancellation model

- The GUI assigns `requestId`/`jobId`/`runId` at submit time; the engine
  assigns its own `job-N` in `accepted`. `EngineWorkerClient` keeps both
  maps, so cancel and event correlation never depend on engine naming.
- `cancel` before `accepted` is recorded and fired the moment the engine job
  id arrives (engine processes commands in order, so this races nothing).
- `engine-worker-exit` (stdout EOF) synthesizes `worker_exit` error events
  for every unfinished job — the Job Tray can never show a phantom
  "running" job — and the app falls back to the one-shot capability
  envelope (analyze disabled) until the worker is restarted.

## 3. Evidence

- `cargo test`: 47 passed, 0 failed (includes new unit tests for command
  wire shape, validation rejects, event dispatch routing, frame-asset
  export/cache/commit, fake-worker death and malformed-line handling).
- `cargo clippy --all-targets -- -D warnings`: clean.
- `GETNATIVE_ENGINE_PATH=<worker build> cargo test worker:: -- --ignored`:
  - `real_engine_worker_roundtrip`: hello → capabilities (validated,
    `analyze=true`) → 21-candidate height analyze with accepted/progress/
    result and telemetry → second identical job reports
    `plan_cache_hits >= 1` (session plan cache materializes through the
    Tauri transport, the E0 goal) → clean shutdown.
  - `media_asset_to_worker_analyze_roundtrip`: real PNG → `frame_asset_still`
    f32le export → worker analyze → 3 real candidate metrics.
- `npm run build` (tsc + vite): clean. `npm test` (vitest): 14 passed,
  including wire→semantic translation, engine-id correlation, deferred and
  immediate cancel, exit synthesis, and rejected-submission id hygiene.
- `npm run test:locale`: parity OK (336 keys; no new user-facing strings in
  this lane).

## 4. Boundaries (recorded, not simulated)

- Video frame-asset export compiles and reuses the shipped FFmpeg
  invocation pattern, but this host has no FFmpeg/fixture, so the video leg
  is covered only by unit tests (cache keys, byte-count commit guard). Run
  the media smoke fixture on a staged host before GUI-3 acceptance on video
  Samples.
- GUI click-through (AnalyzePage form → RunGroup → live plot) is GUI-3
  scope; this lane delivers the transport and Job Tray plumbing it gates
  on. The Job Tray renders live job state as soon as a page submits via
  `engineWorker.submitHeight`.
- `runGroupPlan.ts` orchestration is unchanged; multi-member RunGroups are
  a GUI-3 concern. The transport already serializes engine jobs (worker
  executes one at a time) and preserves per-member job identity.
