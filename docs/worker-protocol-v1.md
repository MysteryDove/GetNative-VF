# Worker Protocol v1

- Status: Implemented (engine side, height/kernel, streamed verify, and engine
  media decode modes).
- Date: 2026-08-07 (v1); 2026-08-10 (v1.2 ring transport and telemetry);
  2026-08-11 (v1.3 engine decode and GPU decode-direct-compute);
  2026-08-12 (v1.4 bounded media verification concurrency).
- Roadmap anchor: `docs/dsmvc-port-strategy.md` phases E0 (worker) and
  E2 (verification pipeline). E2 evidence:
  `docs/performance/e2-verification-pipeline-20260808.md`.
- App-side semantics: `app/src/engine/protocol.ts`. Wire field names in this
  document are authoritative for the engine/protocol integration lane.

## 1. Transport and framing

The engine runs as a long-lived worker process:

```sh
getnative-engine worker
```

- Commands are read from **stdin**, events are written to **stdout**, one
  JSON object per line (JSON Lines, UTF-8). Logs and diagnostics go to
  **stderr**; stdout stays machine-readable.
- Every message carries `"protocol_version": 1`.
- Commands are processed in submission order. `analyze` jobs execute
  one at a time on the executor thread; further commands (including
  `cancel` and `shutdown`) are read while a job runs.
- Bulk pixel data never crosses JSON (GUI contract). Frames travel as
  bounded file assets referenced by path (see §5).

## 2. Commands

### 2.1 `hello`

```json
{"protocol_version": 1, "type": "hello", "request_id": "req-1"}
```

Negotiates protocol and engine version. Must be the first command. Any
command received before `hello` fails with `protocol_error`.

### 2.2 `capabilities`

```json
{"protocol_version": 1, "type": "capabilities", "request_id": "req-2"}
```

Returns the same envelope as the one-shot `capabilities` CLI command.
Inside a worker session, `commands.analyze` and the CPU backend's
`analysis_command_available` report `true`.

Each backend may advertise `auto_priority`. Current priorities are CUDA `10`,
discrete-GPU Vulkan `20`, and CPU `100`; unavailable or Auto-ineligible
backends report `null`. Vulkan also reports `device_type` as one of
`discrete_gpu`, `integrated_gpu`, `virtual_gpu`, `cpu`, or `other`. These
fields are optional for compatibility with older engines.

The optional `features` object advertises `verify_frame_ring`,
`media_frame_batch`, `verify_engine_decode`, and
`media_verify_concurrency` (`min: 1`, `max: 8`, `default: 2`). Missing or false flags are
not errors. `media_frame_batch` names the removed Tauri-side producer and is
reported false by current engines; indexed media uses the commands in section
2.8. Product runtime does not fall back to external FFmpeg executables.

When `verify_engine_decode=true`, optional `decode_backends` entries describe
`software`, `nvdec`, and `vulkan_video`. Each entry contains `compiled`,
`runtime_device`, `codecs`, and `zero_copy`, plus an optional `reason` when
unavailable. `codecs` is device/runtime capability, not a promise that every
profile, level, bit depth, surface allocation, or source file will use hardware
decode. Consumers must use result provenance as the record of what a job
actually used.

### 2.3 `analyze`

Submits one analysis job. v1 implements `mode: "height"` on CPU and explicit
CUDA/Vulkan backends (`backend: "cpu" | "cuda" | "vulkan" | "auto"`). `auto`
prefers CUDA when the device is available and the requested metric is
supported, then a compatible discrete-GPU Vulkan device for p=1, then CPU.
Integrated, virtual, and software Vulkan devices remain explicitly selectable
but do not participate in Auto. Explicit Vulkan currently supports p=1. Other
modes/backends fail with `unsupported`.
Inside a worker session, the capability envelope reports
`analysis_command_available=true` for CPU always and for CUDA/Vulkan when a
compatible device is usable.

```json
{
  "protocol_version": 1,
  "type": "analyze",
  "request_id": "req-3",
  "mode": "height",
  "frame_asset": {
    "path": "/absolute/path/frame.f32",
    "format": "f32le",
    "width": 1920,
    "height": 1080
  },
  "axis_mode": "h_only",
  "kernel": {"id": "bicubic", "b": 0.0, "c": 0.5},
  "candidates": ["710", "711", "712"],
  "metric": {
    "crop_left": 10, "crop_right": 10, "crop_top": 10, "crop_bottom": 10,
    "threshold": 0.015, "p_norm": 1
  },
  "profile_id": "muf-d278cd3",
  "endpoint_rule": "inclusive",
  "base_height": null,
  "base_width": null,
  "grid": {"start": "710", "stop": "712", "step": "1"},
  "backend": "cpu",
  "worker_count": 0
}
```

Field rules:

- `frame_asset.format` is `f32le`: raw little-endian float32, row-major,
  tightly packed (stride = width), exactly `width * height * 4` bytes.
  The producer (Tauri media layer today, engine decode later) owns
  creation and eviction of the file; the engine only reads it.
- `axis_mode`: `h_only` | `w_only` | `h_plus_w`. For `h_only`/`w_only`
  each candidate produces one axis plan; for `h_plus_w` the analysis runs
  the fixed inverse H→V two-axis path per candidate.
- `kernel.id`: one of `bilinear`, `bicubic`, `lanczos`, `spline16`,
  `spline36`, `spline64`. Bicubic requires explicit finite `b` and `c`;
  Lanczos requires explicit integer `taps` in `1..15`. Irrelevant parameters
  must be omitted.
- `profile_id` selects the compatibility contract: `muf-d278cd3`,
  `getfnative-44c8d0f`, or `modern`. Capabilities expose each profile's
  grid arithmetic, defaults, endpoint rule, strict threshold comparison,
  and kernel parameters. The MUF profile is the GUI default.
- `endpoint_rule` is `inclusive` or `exclusive_stop` and applies to `grid`.
  `grid` is optional for legacy requests; when present its decimal sequence
  must have the same length as `candidates` and is generated with the
  selected profile's semantics: MUF repeated addition, getnative index
  multiplication, or modern decimal fixed point. Candidate decimal strings
  remain the result IDs; binary float formatting is never used for IDs.
- `base_height` and `base_width` are optional positive integer decimal
  strings. They select the parity canvas used by the shared geometry resolver.
  Fractional candidates retain `active_length` and their centered fractional
  `shift`; the worker does not truncate them or force shift to zero. H+W
  derives the secondary axis from height/aspect ratio unless `base_width` is
  explicit.
- `candidates`: non-empty array of decimal strings (e.g. `"810"`,
  `"810.5"`). Values must be finite, `>= 2`, and below the source axis
  length. Every resulting `AxisPlanRequest` carries the resolved
  `destination_size`, `active_length`, and `shift` to all selected backends.
- `metric.p_norm`: CPU accepts every positive integer in `1..4294967295`.
  CUDA accepts `1..4`; `auto` uses CUDA for `1..4` when a compatible device is
  available. For p=1, failed or unavailable CUDA initialization continues to
  an Auto-eligible discrete Vulkan device before CPU. For p=2..4, unavailable
  CUDA falls back to CPU; p>4 always uses CPU. Explicit Vulkan accepts p=1
  only. An invalid explicit accelerator/norm combination fails at submission
  with `unsupported` and is never silently rewritten.
- `worker_count`: an explicit positive value wins, bounded by hardware and
  candidate count. CPU auto uses at most 8 workers when the maximum plan
  half-bandwidth is `<=2`, otherwise at most 16. CPU threads and workspaces
  persist across chunks and jobs. On CUDA or Vulkan this field sets candidate
  pipeline depth (1..8; default 3); both retain 32-candidate chunks.

### 2.3.1 `analyze` with `mode: "kernel"` (v1.1)

A kernel scan inverts the height scan: **one fixed geometry, many
kernels**. The wire shape drops `kernel`/`candidates` for a single
`candidate` decimal plus an ordered `kernels` array:

```json
{
  "protocol_version": 1,
  "type": "analyze",
  "request_id": "req-20",
  "mode": "kernel",
  "frame_asset": {"path": "/absolute/path/frame.f32", "format": "f32le",
                  "width": 1920, "height": 1080},
  "axis_mode": "h_only",
  "candidate": "810",
  "kernels": [
    {"id": "bilinear"},
    {"id": "bicubic", "b": 0.0, "c": 0.5},
    {"id": "bicubic", "b": 0.0, "c": 1.0},
    {"id": "lanczos", "taps": 3}
  ],
  "metric": {"p_norm": 1},
  "backend": "cpu"
}
```

Field rules:

- `candidate` is one decimal with the same profile, endpoint, base-dimension,
  and fractional geometry semantics as a height candidate; for `h_plus_w`
  the secondary axis derives per kernel from the aspect ratio exactly like
  height mode.
- `kernels` entries follow the height-mode `kernel` rules (`b`/`c`
  bicubic-only, `taps` lanczos-only, and all applicable parameters required).
  Duplicates are legal (a bicubic (b, c) grid needs them). Cap: 4096.
- Sending both `kernel` and `kernels`, or omitting `candidate`, fails
  `bad_request`.

Result payload: `mode: "kernel"`, `candidate` echo, and one entry per
kernel **in request order**: `{"id": "<index>", "error": …, "kernel":
{…echo with defaults filled…}}`. The `id` is the decimal index into the
request's `kernels` list, so duplicated specs stay unambiguous. Plans
share the session cache and store with height mode (same PlanKey space —
a kernel-mode entry already scanned as a height candidate is a hit).
Cancel/progress semantics are identical to height mode (partial payload
carries the completed prefix in request order).

### 2.4 `cancel`

```json
{"protocol_version": 1, "type": "cancel", "request_id": "req-4", "job_id": "job-3"}
```

Requests cooperative cancellation of a queued or running job. Cancellation
between plan-build chunks and candidate chunks preserves completed
candidate results; the job ends with a `cancelled` event carrying
`"partial": true` when any results were produced, otherwise
`"partial": false`. Cancelling an unknown or finished job id is a no-op
that still emits `cancelled` with `"partial": false` and
`detail: "not_running"`.

### 2.5 `shutdown`

```json
{"protocol_version": 1, "type": "shutdown", "request_id": "req-5"}
```

Emits a final `shutdown` acknowledgement event, then the process exits
with status 0. Running jobs are cancelled cooperatively first.

### 2.6 `verify_begin` / `verify_frame` / `verify_end` (v1.1)

Verification is the inverse workload of `analyze`: **one locked recipe,
many frames**. Frames stream in as the producer (decode) makes them
available, so analysis overlaps decode; per-frame results stream back in
`progress` events.

`verify_begin` submits the recipe and queues the job (jobs still execute
one at a time on the executor thread):

```json
{
  "protocol_version": 1,
  "type": "verify_begin",
  "request_id": "req-10",
  "geometry": {"width": 1920, "height": 1080},
  "axis_mode": "h_only",
  "kernel": {"id": "bicubic", "b": 0.0, "c": 0.5},
  "candidate": "810",
  "metric": {"crop_left": 10, "crop_right": 10, "crop_top": 10,
             "crop_bottom": 10, "threshold": 0.015, "p_norm": 1},
  "backend": "cpu",
  "worker_count": 0,
  "expected_frames": 34500
}
```

Field rules:

- `geometry` gives the frame dimensions every streamed asset must match.
- `candidate` is a single decimal with the same plan semantics as one
  `analyze` candidate (primary axis from `axis_mode`; the secondary axis
  in `h_plus_w` mode is aspect-derived exactly like height mode).
- `backend`: verify is **CPU-only** in v1.1 (`cpu`/`auto`); `cuda` and `vulkan`
  fail with `unsupported` rather than silently falling back.
- `worker_count`: 0 selects the default, `min(16, hardware)`.
- `expected_frames` (optional, 1..1,000,000) is the progress total; the
  integrity total is the `verify_end` declaration instead.

The `accepted` event carries `mode: "verify"`, `worker_count`, and
`suggested_in_flight` (2× workers). The engine does not hard-cap the
inbound frame queue — queue items are small paths and frame files are
loaded just-in-time by the analysis workers. The producer should bound
its *asset creation* to `suggested_in_flight` unacknowledged frames so
on-disk assets stay bounded.

```json
{"protocol_version": 1, "type": "verify_frame", "request_id": "req-11",
 "job_id": "job-7", "seq": 0,
 "frame_asset": {"path": "/absolute/path/f0.f32", "format": "f32le",
                 "width": 1920, "height": 1080}}
```

- `seq` must be contiguous from 0 (a gap fails `bad_request`; the stream
  stays usable and the correct `seq` can be sent next).
- The asset geometry must match the recipe.
- Frames may repeat paths (a ring of decoded buffers is a valid stream).
- Analysis workers map each asset read-only (`mmap`, POSIX) for its
  single use; producers must publish assets atomically (tmp+rename), as
  the Tauri media layer already does. Windows currently reads into a
  per-worker buffer (documented tradeoff in the E2 evidence).

```json
{"protocol_version": 1, "type": "verify_end", "request_id": "req-12",
 "job_id": "job-7", "total": 34500}
```

`total` declares the stream length. If it differs from the received
frame count, the command fails `bad_request` **and** the job is
cancelled with `detail: "verify_total_mismatch"` (event order between
the two is unspecified). `verify_frame`/`verify_end` for an unknown,
mismatched-type, or finished job fail `bad_request`.

Per-frame failures (missing/corrupt asset) do not fail the job: the
engine emits a `warning` event with `code: "frame_asset_error"` and the
offending `seq`, records a `null` error for that seq, and continues —
this is the mixed-success-batch semantics GUI-5 needs.

Per-frame results stream in `progress` events with `phase: "verify"`,
batched (up to 64 results and never above `suggested_in_flight`, plus a final
drain), each entry
`{"seq": N, "error": <number|null>}`. Entries may arrive out of order;
consumers key on `seq`. Resume is app-side: the app keeps streamed
results and re-issues only missing seqs on a later job.

The terminal `result` payload:

```json
{
  "mode": "verify",
  "frames_completed": 34498,
  "frames_failed": 2,
  "telemetry": {
    "plan_build_count": 0, "plan_cache_hits": 1,
    "plan_resident_entries": 37, "plan_ms": 0.31,
    "frame_load_ms": 4389.0, "frame_analyze_ms": 5586.0,
    "stream_ms": 31350.0, "fps": 1100.5,
    "worker_count": 16, "backend": "cpu", "isa": "avx2"
  }
}
```

When `features.verify_frame_ring=true`, the producer may instead attach one
fixed-size shared file after `accepted` and before the first frame:

```json
{"protocol_version":1,"type":"verify_ring_attach","request_id":"req-r0",
 "job_id":"job-7","path":"/absolute/path/verify.ring",
 "slot_count":16,"frame_bytes":8294400}
```

Each ring frame carries only slot identity. `seq` remains contiguous; `slot`
must be in range and `generation` must strictly increase on every reuse.

```json
{"protocol_version":1,"type":"verify_frame","request_id":"req-r1",
 "job_id":"job-7","seq":0,"slot":0,"generation":1}
```

After reading and analyzing the slot, the worker emits `verify_consumed` with
the same `seq`, `slot`, and `generation`. Only that exact tuple releases the
slot. The native producer waits when the ring is full; cancel, decode failure,
worker exit, and terminal events wake waiters and remove the mapping/file. The
frontend clamps the accepted hint to 2..64; the engine itself supports one slot
for protocol-level callers.

`frame_load_ms`/`frame_analyze_ms` accumulate across workers (divide by
frames for per-frame cost). Cancellation mid-stream preserves streamed
results and ends with `cancelled` carrying `partial`, `frames_completed`,
`frames_failed`; the last result batch drains before the `cancelled`
event, so streamed results always equal the payload counters.

### 2.7 `verify_media_begin`

`verify_media_begin` is an additive engine-decode command. It carries the same
locked recipe as `verify_begin`, plus `media` (`path`, `fingerprint`,
`stream_index`) and `scan_scope` (`selection`, `every_n`, inclusive start/end).
The worker emits `accepted` before indexing, then publishes the exact selected
frame total in `progress` once presentation-order indexing finishes. This keeps
preparation cancellable without guessing a total.

```json
{
  "protocol_version": 1,
  "type": "verify_media_begin",
  "request_id": "req-media-1",
  "geometry": {"width": 1920, "height": 1080},
  "axis_mode": "h_only",
  "kernel": {"id": "bicubic", "b": 0.0, "c": 0.5},
  "candidate": "810",
  "metric": {"crop_left": 10, "crop_right": 10,
             "crop_top": 10, "crop_bottom": 10,
             "threshold": 0.015, "p_norm": 1},
  "backend": "auto",
  "concurrency": 2,
  "media": {"path": "/absolute/path/source.mkv",
            "fingerprint": "quick-sha256-v1:...", "stream_index": 0},
  "scan_scope": {"selection": "every_n", "every_n": 5,
                 "start_frame": 100, "end_frame": 500}
}
```

`selection` is `all`, `every_n`, or `decoded_i_picture`. Frame identity and
range bounds use presentation-order indices; start/end are optional and
inclusive. `fingerprint` may be empty, but when present must match the engine's
`quick-sha256-v1` result or the job fails with
`media_fingerprint_error`. A resolution change or a mismatch with locked
`geometry` is a content error, not a capability fallback.

`concurrency` is optional, defaults to 2, and must be an integer in `1..8`.
It is the maximum number of selected frames simultaneously queued or being
analyzed inside one media job. It does not change frame selection, does not
schedule multiple Sources concurrently, and is independent of the legacy
streamed verify `worker_count`. The `accepted` event echoes `concurrency`.

Decode and analysis use one bounded pipeline for all three selection modes.
Completed frames may arrive internally out of order, but progress batches and
the final `payload.frames` are emitted in selected-frame `seq` order. CUDA
surfaces and Vulkan frame locks remain leased until their analysis completes.
Before selected-frame decode starts, CUDA/Vulkan validate execution slots,
workspace, storage-buffer limits, and aggregate device-memory requirements.
Failure returns `media_concurrency_unavailable`; it never lowers concurrency or
tries another compute backend.

The command selects compute first. Explicit `cuda` and `vulkan` must initialize
that compute backend or fail with `unsupported`; decoder capability never
silently changes an explicitly requested compute backend. `auto` tries CUDA,
then an eligible discrete Vulkan device for p=1, then CPU, emitting
`compute_backend_fallback` entries as initialization attempts fail.

CUDA attempts FFmpeg's generic CUDA hardware configuration on the analysis
engine's exact `CUcontext`; decoder surfaces are converted from
NV12/P010/YUV444 luma directly into the resident F32 source buffer. Vulkan
uses FFmpeg's generic Vulkan configuration on the analysis engine's exact
`VkInstance`/`VkPhysicalDevice`/`VkDevice`, waits on the decoded frame's
timeline semaphore, converts its luma image to the resident F32 storage
buffer, and updates the frame synchronization state before release. Vulkan
Video is considered usable only with Vulkan 1.3, a video decode queue,
timeline semaphores, and the matching codec decode extension. Vulkan compute
remains limited to p=1.

Unsupported or failed hardware decode emits one `warning` with
`code=hardware_decode_fallback` and `from`, `to`, `reason`, and `frame_seq`,
then continues with software decode and upload to the selected compute
backend. A mid-stream hardware failure restarts software decode after the
completed prefix; result sequence numbers are neither duplicated nor skipped.
Legacy `verify_begin`/`verify_frame`/`verify_end` remains unchanged.

Result provenance records requested/actual compute backend, actual decoder,
codec/profile, device UUID, surface format, bit depth/range, `zero_copy`, and
the fallback chain. `decoder` is `software`, `nvdec`, or `vulkan_video`.
Zero-copy means the decoded full frame never enters host memory; both
`host_frame_bytes` and `source_upload_bytes` are zero. Device luma conversion,
plan upload, and scalar metric readback remain allowed.

Engine-decode telemetry reports `index_ms`, `decode_ms`, `decoded_frames`, `convert_ms`,
`upload_ms`, `compute_ms`, `readback_ms`, `host_frame_bytes`,
`conversion_bytes`, `source_upload_bytes`, `plan_upload_bytes`,
`result_readback_bytes`, `job_total_ms`, `fps`, `requested_concurrency`,
`effective_concurrency`, `max_inflight`, `execution_slot_wait_ms`,
`queue_wait_ms`, and `surface_lease_peak`. Hardware zero-copy uses
device conversion bytes while software decode plus GPU compute records host
frame and source upload bytes.

### 2.8 Indexed media commands

Worker protocol v1 also advertises four asynchronous media commands:
`media_index_begin`, `media_frame_window`, `media_preview_begin`, and
`media_asset_batch_begin`. Each takes `request_id`, an absolute `path`, an
optional `fingerprint` and `stream_index`, plus an application-owned
`cache_directory`. They use the dedicated media queue and emit the normal
`accepted`/`progress`/`result` sequence; `cancel` targets the accepted
`job_id`. Identical concurrent index requests for path/fingerprint/stream
share one engine job and receive separate terminal events.

`media_index_begin` chooses the lowest video stream index when `stream_index`
is absent. It records decoded `AVFrame` output identities in the private binary
`GNVFLWI\0` format. The default stream uses `<media>.gnvf.lwi`; a saved legacy
stream uses `<media>.stream-N.lwi`. A cache-directory index is used when the
media directory is not writable. Version, source size/mtime/fingerprint, frame
table checksum, stream/codec/timebase, timestamps, picture type, keyframe flag,
and nearest keyframe anchor are validated on every load; invalid files rebuild
atomically and cancelled builds leave no valid temporary index.

`media_frame_window` resolves `target` (`frame`, `timestamp`,
`previous_keyframe`, or `next_keyframe`) and returns presentation-order frame
identities. `media_preview_begin` returns one cached PNG asset. The engine seeks
with `avformat_seek_file`, flushes the codec, and decodes from the indexed
keyframe anchor; only streams without usable timestamps may decode from the
beginning.

`media_asset_batch_begin` accepts 1..26 unique `item_id` entries in `assets`.
Each entry selects `f32le` or `png`; PNG entries also carry
`maximum_dimension`. Assets are grouped by indexed keyframe anchor so each GOP
is decoded once. Cache keys include fingerprint, stream, frame, dimensions,
format, and index version. Product code never launches `ffmpeg` or `ffprobe`;
those executables may be used only by test fixture generators.

## 3. Events

Every event carries `protocol_version`, `request_id` (of the triggering
command), `job_id` (for job events), and `timestamp_ms` (UTC epoch
milliseconds).

| `type` | Meaning | Extra fields |
| --- | --- | --- |
| `hello_ok` | Protocol negotiated | `engine_version`, `commands` |
| `capabilities` | Capability envelope | `payload` (same schema as one-shot CLI) |
| `accepted` | Job queued after backend initialization | `mode`, `job_id`, optional worker-confirmed `backend` (`cpu`, `cuda`, or `vulkan`) and accelerator `device`; verify adds `worker_count`, `suggested_in_flight` |
| `progress` | Job progress | `completed`, `total`, `phase` (`plan`, `candidates`, or `verify`); verify adds `results` batches of `{seq, error\|null}` |
| `verify_consumed` | Ring slot may be reused; consumed internally by Tauri | `seq`, `slot`, `generation` |
| `warning` | Non-fatal issue (e.g. backend fallback, verify frame failure) | `code`, `message`; verify frame failures add `seq` |
| `result` | Job finished successfully | `mode`, `payload` (§4) |
| `cancelled` | Job cancelled | `partial`, `payload` (partial results when `partial=true`) |
| `error` | Command/job failed | `code`, `message`, `retryable` |
| `shutdown` | Worker exiting | — |

Error codes: `protocol_error`, `bad_request`, `busy` (reserved; v1 queues
instead), `unsupported`, `frame_asset_error`, `cancelled` (reserved),
`internal`. `retryable` is true only for transient I/O failures.

## 4. Result payload (height mode)

```json
{
  "mode": "height",
  "candidates": [
    {"id": "710", "error": 1234.5678},
    {"id": "711", "error": 1201.2345}
  ],
  "telemetry": {
    "plan_build_count": 3,
    "plan_cache_hits": 0,
    "plan_ms": 12.34,
    "candidate_ms": 56.78,
    "job_total_ms": 70.12,
    "backend": "cpu",
    "isa": "avx2"
  }
}
```

`candidates` retains input order, including duplicate candidate ids (each
occurrence is computed and reported; the plan cache deduplicates the
build). Raw zero stays zero; log display transforms are a UI concern.

Stable timing keys are `frontend_queue_ms`, `media_decode_ms`,
`asset_wait_ms`, `worker_queue_ms`, `plan_ms`, `candidate_ms`, and
`job_total_ms`; ring verify also reports `ring_wait_ms`. Engine payloads include
plan/asset/cache hit and miss counts. The frontend adds `frontend_queue_ms` by
copying payload and telemetry objects, never by mutating the wire event.

## 5. Session state

The worker owns one `AxisPlanCache` (default limits: 1024 entries /
256 MiB) for the whole session. Successive `analyze` jobs that share plan
keys (same sample at new heights, RunGroup members, verification frames)
hit warm plans without rebuilding; verify jobs reuse the same cache for
their locked recipe (a recipe already scanned in the session starts with
`plan_cache_hits=1`). Cache residency is reported in
`telemetry`.

The persistent L2 plan store is enabled by default and writes `.gnpk` packs to
the per-user cache on Linux: `$XDG_CACHE_HOME/io.getnative.vf/axis-plans`, or
`$HOME/.cache/io.getnative.vf/axis-plans` when `XDG_CACHE_HOME` is unset. The
Tauri app passes its equivalent `app_cache_dir()/axis-plans` path explicitly.
Other platforms default to the directory containing `getnative-engine`.
`GETNATIVE_PLAN_CACHE_DIR=<dir>` overrides the location;
`GETNATIVE_PLAN_CACHE=off` disables L2 persistence without disabling the L1
session cache. Store creation, read, or write failures degrade to plan rebuilds
and do not fail a job.

Frame assets are held in an 8-entry session frame cache (LRU); buffers
are never resized after load, so backend-visible frame pointers are
stable across jobs. The CUDA backend additionally keeps the uploaded
device frame resident per execution slot (`SourceIdentity`: pointer +
geometry + content probe) and skips re-upload/re-transpose on hits; CUDA
telemetry fields (`cuda_source_upload_bytes`, `cuda_plan_upload_bytes`,
`cuda_source_cache_hits`, `cuda_kernel_ms`, `cuda_gpu_total_ms`,
`cuda_device`, …) are merged into the job result telemetry when
`backend: "cuda"` is used.

The Vulkan backend uses the same worker chunk/pipeline/cancellation schedule.
Its own two-slot runtime tiles further only when required by
`maxStorageBufferRange` or the configured workspace limit. Vulkan telemetry
includes command submissions, dispatches, tiles, plan/source transfer bytes,
host pack time, GPU execution time, slot wait time, and device name.

All CUDA execution slots share one immutable device-input LRU and upload
stream; the recommended enabled budget is `min(512 MiB, 10% device memory)`.
It is benchmark-gated off by default after a formal bilinear warm regression;
`GETNATIVE_CUDA_INPUT_CACHE_BYTES=<bytes>` opts in and `=0` disables it.
Telemetry includes source upload and transpose counts, which should both be
one for a repeated same-source H+W scan. Host packed-plan caching is
implemented but benchmark-gated off by default because the formal bilinear
matrix regressed; `GETNATIVE_CUDA_HOST_PLAN_CACHE_BYTES=<bytes>` opts in.

## 6. App media integration

Tauri forwards `media_index_begin`, `media_frame_window`,
`media_preview_begin`, and `media_asset_batch_begin` to the resident engine and
injects the application cache directory. The frontend listens for the normal
worker events before submission, cancels stale accepted jobs by `job_id`, and
reads only engine-produced PNG files below that cache directory. Video frame
assets for analysis use `media_asset_batch_begin`; the retained
`media_frame_asset` command handles still images only. There is no Tauri media
producer, ticket/ack side channel, or executable FFmpeg fallback.

## 7. Reproducible probe

`engine/bench/pipeline_probe.py <engine>` creates a real FFmpeg fixture and
runs resident-worker height/kernel/verify plus an 8-frame serial-versus-batch
media export comparison. The default matrix uses 1080p, 301 candidates,
CPU 1/8/16/auto, available CUDA p-norms `1..4`, 1,000/10,000 verify frames,
and five samples (`--p-norms` overrides the CUDA norm matrix);
`--quick` is the smoke path. JSON output records cold/warm wall time, stable
timings including CUDA kernel/metric time, RSS/HWM, upload bytes,
command/event counts, cache counters, checksums, and candidate rankings.

## 8. Relationship to the one-shot CLI

`capabilities` and `geometry` remain available as one-shot commands for
the current Tauri synchronous transport. The worker command is additive;
once the Tauri lane adopts the worker transport, capability gating reads
`commands.analyze=true` from the worker `capabilities` event.
