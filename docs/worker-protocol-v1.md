# Worker Protocol v1

- Status: Implemented (engine side, height mode, CPU backend).
- Date: 2026-08-07.
- Roadmap anchor: `docs/dsmvc-port-strategy.md` phase E0.
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

### 2.3 `analyze`

Submits one analysis job. v1 implements `mode: "height"` on the CPU and
CUDA backends (`backend: "cpu" | "cuda" | "auto"`; `auto` selects CPU,
the deterministic oracle). Other modes/backends fail with `unsupported`.
Inside a worker session, the capability envelope reports
`analysis_command_available=true` for CPU always and for CUDA when a
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
  `spline36`, `spline64`. `b`/`c` apply to bicubic only; `taps` (integer,
  1..15) applies to lanczos only. Irrelevant parameters must be omitted.
- `candidates`: non-empty array of decimal strings (e.g. `"810"`,
  `"810.5"`). Each candidate maps to an axis plan with
  `destination_size = floor(value)`, `active_length = value`, `shift = 0`
  (v1 geometry semantics; fractional `src_top`/`src_left` shifts are a
  v1.1 refinement). Values must be finite, `>= 2`, and below the source
  axis length.
- `metric.p_norm`: currently `1` for CPU parity with the validated
  fixtures; other values fail with `unsupported`.
- `worker_count`: 0 selects the backend default.

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

## 3. Events

Every event carries `protocol_version`, `request_id` (of the triggering
command), `job_id` (for job events), and `timestamp_ms` (UTC epoch
milliseconds).

| `type` | Meaning | Extra fields |
| --- | --- | --- |
| `hello_ok` | Protocol negotiated | `engine_version`, `commands` |
| `capabilities` | Capability envelope | `payload` (same schema as one-shot CLI) |
| `accepted` | Job queued | `mode`, `job_id` |
| `progress` | Job progress | `completed`, `total`, `phase` (`plan` or `candidates`) |
| `warning` | Non-fatal issue (e.g. backend fallback) | `code`, `message` |
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
    "candidates_ms": 56.78,
    "backend": "cpu",
    "isa": "avx2"
  }
}
```

`candidates` retains input order, including duplicate candidate ids (each
occurrence is computed and reported; the plan cache deduplicates the
build). Raw zero stays zero; log display transforms are a UI concern.

## 5. Session state

The worker owns one `AxisPlanCache` (default limits: 1024 entries /
256 MiB) for the whole session. Successive `analyze` jobs that share plan
keys (same sample at new heights, RunGroup members, verification frames)
hit warm plans without rebuilding. Cache residency is reported in
`telemetry`. There is no cross-process persistence in v1.

Frame assets are held in an 8-entry session frame cache (LRU); buffers
are never resized after load, so backend-visible frame pointers are
stable across jobs. The CUDA backend additionally keeps the uploaded
device frame resident per execution slot (`SourceIdentity`: pointer +
geometry + content probe) and skips re-upload/re-transpose on hits; CUDA
telemetry fields (`cuda_source_upload_bytes`, `cuda_plan_upload_bytes`,
`cuda_source_cache_hits`, `cuda_kernel_ms`, `cuda_gpu_total_ms`,
`cuda_device`, …) are merged into the job result telemetry when
`backend: "cuda"` is used.

## 6. Relationship to the one-shot CLI

`capabilities` and `geometry` remain available as one-shot commands for
the current Tauri synchronous transport. The worker command is additive;
once the Tauri lane adopts the worker transport, capability gating reads
`commands.analyze=true` from the worker `capabilities` event.
