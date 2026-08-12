//! Resident engine worker transport (worker protocol v1).
//!
//! Owns the long-lived `getnative-engine worker` child process: JSONL
//! commands on stdin, events on stdout, diagnostics on stderr. Request /
//! response commands (hello, capabilities) are awaited internally; job
//! events (accepted/progress/result/cancelled/error) are forwarded to the
//! frontend through Tauri events. Bulk pixels never cross this channel —
//! frames travel as f32le file assets produced by `media.rs`.
//!
//! Wire field names follow `docs/worker-protocol-v1.md` (snake_case);
//! the TS client maps them onto the camelCase semantic events in
//! `app/src/engine/protocol.ts`.

use crate::engine::{find_engine, validate_capabilities};
use serde::Deserialize;
use serde_json::{json, Map, Value};
use std::collections::{HashMap, VecDeque};
use std::fs;
use std::io::{BufRead, BufReader, BufWriter, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::Duration;
use tauri::{ipc::Response, AppHandle, Emitter, Manager, State};

const PROTOCOL_VERSION: u32 = 1;
const STDERR_TAIL_LINES: usize = 50;
const RESPONSE_TIMEOUT: Duration = Duration::from_secs(30);
const SHUTDOWN_TIMEOUT: Duration = Duration::from_secs(5);
const MAX_FRAME_AXIS: u32 = 65_536;
const MAX_CANDIDATES: usize = 100_000;
const CUDA_MAXIMUM_P_NORM: u32 = 4;
const MAX_CACHED_PREVIEW_BYTES: u64 = 64 * 1024 * 1024;

fn media_cache_directory(app: &AppHandle) -> Result<PathBuf, String> {
    let app_cache = app
        .path()
        .app_cache_dir()
        .map_err(|error| format!("media_cache_error: {error}"))?;
    let legacy = app_cache.join("media-index");
    if let Ok(entries) = fs::read_dir(&legacy) {
        for entry in entries.flatten() {
            let path = entry.path();
            let removable = path.extension().and_then(|value| value.to_str()) == Some("jsonl")
                || path.file_name().and_then(|value| value.to_str())
                    .is_some_and(|value| value.contains(".jsonl.") || value.ends_with(".tmp"));
            if removable {
                let _ = fs::remove_file(path);
            }
        }
        let _ = fs::remove_dir(&legacy);
    }
    let directory = app_cache.join("media");
    fs::create_dir_all(&directory)
        .map_err(|error| format!("media_cache_error: failed to create cache directory: {error}"))?;
    directory
        .canonicalize()
        .map_err(|error| format!("media_cache_error: failed to resolve cache directory: {error}"))
}

/// Output of the stdout reader thread. Kept transport-agnostic so the
/// session logic is testable without a running Tauri application.
#[derive(Debug)]
pub enum WorkerOutput {
    /// One protocol event line from the worker.
    Event(Value),
    /// stdout reached EOF: the worker exited or crashed.
    Exited { stderr_tail: Vec<String> },
}

pub type WorkerSink = Arc<dyn Fn(WorkerOutput) + Send + Sync>;

type PendingMap = Arc<Mutex<HashMap<String, mpsc::Sender<Value>>>>;

struct WorkerSession {
    child: Child,
    stdin: BufWriter<ChildStdin>,
    pending: PendingMap,
    next_internal_request: AtomicU64,
    engine_path: PathBuf,
    hello: Value,
}

impl WorkerSession {
    fn alive(&mut self) -> bool {
        matches!(self.child.try_wait(), Ok(None))
    }

    fn next_request_id(&self) -> String {
        format!("tauri-internal-{}", self.next_internal_request.fetch_add(1, Ordering::Relaxed))
    }

    fn write_command(&mut self, command: &Value) -> Result<(), String> {
        let line = serde_json::to_string(command)
            .map_err(|error| format!("worker_protocol_error: failed to encode command: {error}"))?;
        self.stdin
            .write_all(line.as_bytes())
            .and_then(|()| self.stdin.write_all(b"\n"))
            .and_then(|()| self.stdin.flush())
            .map_err(|error| format!("worker_io_error: failed to send command to the engine worker: {error}"))
    }

    /// Send a command and wait for the event that carries its request id.
    fn roundtrip(&mut self, mut command: Value) -> Result<Value, String> {
        let request_id = self.next_request_id();
        command["request_id"] = json!(request_id);
        let (tx, rx) = mpsc::channel();
        self.pending
            .lock()
            .map_err(|_| "worker_internal_error: pending map poisoned".to_owned())?
            .insert(request_id.clone(), tx);
        let result = self.write_command(&command).and_then(|()| {
            rx.recv_timeout(RESPONSE_TIMEOUT).map_err(|_| {
                format!("worker_timeout: the engine worker did not answer within {RESPONSE_TIMEOUT:?}")
            })
        });
        if result.is_err() {
            if let Ok(mut pending) = self.pending.lock() {
                pending.remove(&request_id);
            }
        }
        let response = result?;
        if response.get("type").and_then(Value::as_str) == Some("error") {
            let code = response.get("code").and_then(Value::as_str).unwrap_or("internal");
            let message = response
                .get("message")
                .and_then(Value::as_str)
                .unwrap_or("unknown engine worker error");
            return Err(format!("worker_{code}: {message}"));
        }
        Ok(response)
    }

    fn terminate(&mut self) {
        let _ = self.write_command(&json!({
            "protocol_version": PROTOCOL_VERSION,
            "type": "shutdown",
        }));
        let deadline = std::time::Instant::now() + SHUTDOWN_TIMEOUT;
        loop {
            match self.child.try_wait() {
                Ok(Some(_)) => return,
                Ok(None) if std::time::Instant::now() < deadline => {
                    std::thread::sleep(Duration::from_millis(25));
                }
                _ => break,
            }
        }
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

impl Drop for WorkerSession {
    fn drop(&mut self) {
        // Never outlive the app: a resident worker without its owner would
        // hold decoder/session resources indefinitely.
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn dispatch_event(value: Value, pending: &PendingMap, sink: &WorkerSink) {
    if let Some(request_id) = value.get("request_id").and_then(Value::as_str) {
        let waiter = pending
            .lock()
            .ok()
            .and_then(|mut pending| pending.remove(request_id));
        if let Some(waiter) = waiter {
            let _ = waiter.send(value);
            return;
        }
    }
    sink(WorkerOutput::Event(value));
}

fn spawn_session(
    engine_path: &Path,
    sink: WorkerSink,
    configured_cache_dir: Option<&Path>,
) -> Result<WorkerSession, String> {
    // Keep the L2 cache enabled for packaged engines, including older staged
    // binaries that predate the engine-side executable-directory default.
    // The preference path is explicit so the GUI and CLI use one location.
    let cache_dir = configured_cache_dir
        .map(Path::to_path_buf)
        .unwrap_or_else(|| engine_path.parent().unwrap_or(Path::new(".")).to_path_buf());
    let mut command = Command::new(engine_path);
    command
        .arg("worker")
        .env("GETNATIVE_PLAN_CACHE", "on")
        .env("GETNATIVE_PLAN_CACHE_DIR", &cache_dir);
    let mut child = command
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|error| format!("worker_spawn_error: failed to start the engine worker: {error}"))?;

    let stdin = child
        .stdin
        .take()
        .ok_or_else(|| "worker_spawn_error: engine worker stdin is unavailable".to_owned())?;
    let stdout = child
        .stdout
        .take()
        .ok_or_else(|| "worker_spawn_error: engine worker stdout is unavailable".to_owned())?;
    let stderr = child
        .stderr
        .take()
        .ok_or_else(|| "worker_spawn_error: engine worker stderr is unavailable".to_owned())?;

    let pending: PendingMap = Arc::new(Mutex::new(HashMap::new()));
    let stderr_tail: Arc<Mutex<VecDeque<String>>> = Arc::new(Mutex::new(VecDeque::new()));

    std::thread::spawn({
        let stderr_tail = Arc::clone(&stderr_tail);
        move || {
            for line in BufReader::new(stderr).lines() {
                let Ok(line) = line else { break };
                if let Ok(mut tail) = stderr_tail.lock() {
                    if tail.len() >= STDERR_TAIL_LINES {
                        tail.pop_front();
                    }
                    tail.push_back(line);
                }
            }
        }
    });

    std::thread::spawn({
        let pending = Arc::clone(&pending);
        let stderr_tail = Arc::clone(&stderr_tail);
        move || {
            for line in BufReader::new(stdout).lines() {
                match line {
                    Ok(line) if line.trim().is_empty() => {}
                    Ok(line) => match serde_json::from_str::<Value>(&line) {
                        Ok(value) => dispatch_event(value, &pending, &sink),
                        Err(error) => sink(WorkerOutput::Event(json!({
                            "protocol_version": PROTOCOL_VERSION,
                            "type": "error",
                            "code": "protocol_error",
                            "message": format!("engine worker emitted a non-JSON line: {error}"),
                            "retryable": false,
                        }))),
                    },
                    Err(_) => break,
                }
            }
            // stdout closed: fail every awaited command, then report the exit.
            if let Ok(mut pending) = pending.lock() {
                pending.clear();
            }
            let stderr_tail = stderr_tail
                .lock()
                .map(|tail| tail.iter().cloned().collect::<Vec<_>>())
                .unwrap_or_default();
            sink(WorkerOutput::Exited { stderr_tail });
        }
    });

    Ok(WorkerSession {
        child,
        stdin: BufWriter::new(stdin),
        pending,
        next_internal_request: AtomicU64::new(1),
        engine_path: engine_path.to_path_buf(),
        hello: Value::Null,
    })
}

fn tauri_sink(app: &AppHandle) -> WorkerSink {
    let app = app.clone();
    Arc::new(move |output| match output {
        WorkerOutput::Event(value) => {
            let _ = app.emit("engine-worker-event", value);
        }
        WorkerOutput::Exited { stderr_tail } => {
            let _ = app.emit("engine-worker-exit", json!({ "stderr_tail": stderr_tail }));
        }
    })
}

#[derive(Default)]
pub struct WorkerManager {
    session: Mutex<Option<WorkerSession>>,
}

impl WorkerManager {
    fn with_live_session<T>(
        &self,
        action: impl FnOnce(&mut WorkerSession) -> Result<T, String>,
    ) -> Result<T, String> {
        let mut guard = self
            .session
            .lock()
            .map_err(|_| "worker_internal_error: session lock poisoned".to_owned())?;
        let alive = match guard.as_mut() {
            Some(session) => session.alive(),
            None => false,
        };
        if !alive {
            // Dead or missing session: drop kills the child, if any.
            guard.take();
        }
        let session = guard.as_mut().ok_or_else(|| {
            "worker_not_running: the engine worker is not running; start it first".to_owned()
        })?;
        action(session)
    }

}

#[tauri::command]
pub fn engine_worker_start(app: AppHandle, state: State<'_, WorkerManager>) -> Result<Value, String> {
    let mut guard = state
        .session
        .lock()
        .map_err(|_| "worker_internal_error: session lock poisoned".to_owned())?;
    if let Some(session) = guard.as_mut() {
        if session.alive() {
            return Ok(session.hello.clone());
        }
        // Dead session: drop kills the child and a fresh one is spawned below.
        guard.take();
    }

    let path: PathBuf = find_engine(&app)?;
    let configured_cache_dir = crate::prefs::load_preferences(&app)
        .ok()
        .and_then(|prefs| prefs.axis_plan_cache_dir.map(PathBuf::from));
    let mut session = spawn_session(&path, tauri_sink(&app), configured_cache_dir.as_deref())?;
    let hello = session.roundtrip(json!({
        "protocol_version": PROTOCOL_VERSION,
        "type": "hello",
    }))?;
    if hello.get("type").and_then(Value::as_str) != Some("hello_ok")
        || hello.get("protocol_version").and_then(Value::as_u64) != Some(PROTOCOL_VERSION as u64)
    {
        return Err("worker_protocol_error: the engine worker rejected the protocol handshake".to_owned());
    }
    session.hello = json!({
        "path": path,
        "payload": hello,
    });
    let result = session.hello.clone();
    *guard = Some(session);
    Ok(result)
}

#[tauri::command]
pub fn engine_worker_capabilities(state: State<'_, WorkerManager>) -> Result<Value, String> {
    state.with_live_session(|session| {
        let response = session.roundtrip(json!({
            "protocol_version": PROTOCOL_VERSION,
            "type": "capabilities",
        }))?;
        if response.get("type").and_then(Value::as_str) != Some("capabilities") {
            return Err("worker_protocol_error: unexpected capabilities response".to_owned());
        }
        let payload = response
            .get("payload")
            .cloned()
            .ok_or_else(|| "worker_protocol_error: capabilities response has no payload".to_owned())?;
        validate_capabilities(&payload)?;
        Ok(json!({ "path": session.engine_path, "payload": payload }))
    })
}

/// Forward one engine-owned media job. The frontend supplies the protocol
/// fields in snake_case; this boundary owns protocol version and cache paths.
#[tauri::command]
pub fn engine_worker_media_begin(
    app: AppHandle,
    state: State<'_, WorkerManager>,
    mut request: Value,
) -> Result<Value, String> {
    let object = request
        .as_object_mut()
        .ok_or_else(|| "bad_request: media request must be an object".to_owned())?;
    let request_id = object
        .get("request_id")
        .and_then(Value::as_str)
        .filter(|value| !value.trim().is_empty())
        .ok_or_else(|| "bad_request: request_id must not be empty".to_owned())?
        .to_owned();
    let command_type = object
        .get("type")
        .and_then(Value::as_str)
        .ok_or_else(|| "bad_request: media request type is required".to_owned())?
        .to_owned();
    if !matches!(
        command_type.as_str(),
        "media_index_begin"
            | "media_frame_window"
            | "media_preview_begin"
            | "media_asset_batch_begin"
    ) {
        return Err(format!("bad_request: unsupported media command {command_type}"));
    }
    let path = object
        .get("path")
        .and_then(Value::as_str)
        .ok_or_else(|| "bad_request: media path is required".to_owned())?
        .to_owned();
    let canonical_path = crate::media::validated_media_path(&path)?;
    object.insert("path".to_owned(), json!(canonical_path));
    object.insert("protocol_version".to_owned(), json!(PROTOCOL_VERSION));
    object.insert(
        "cache_directory".to_owned(),
        json!(media_cache_directory(&app)?),
    );
    state.with_live_session(|session| {
        let available = session
            .hello
            .get("payload")
            .and_then(|payload| payload.get("commands"))
            .and_then(|commands| commands.get(&command_type))
            .and_then(Value::as_bool)
            .unwrap_or(false);
        if !available {
            return Err(
                "video_backend_unavailable: the engine was built without in-process media support"
                    .to_owned(),
            );
        }
        session.write_command(&request)
    })?;
    Ok(json!({ "requestId": request_id, "queued": true }))
}

/// Read only PNG files produced by the engine below the app media cache.
#[tauri::command]
pub fn engine_worker_media_read_asset(
    app: AppHandle,
    path: String,
) -> Result<Response, String> {
    let cache_directory = media_cache_directory(&app)?;
    let requested = PathBuf::from(&path)
        .canonicalize()
        .map_err(|error| format!("media_asset_error: failed to resolve cached asset: {error}"))?;
    if !requested.starts_with(&cache_directory)
        || requested.extension().and_then(|value| value.to_str()) != Some("png")
    {
        return Err("media_asset_error: cached preview path is outside the media cache".to_owned());
    }
    let metadata = fs::metadata(&requested)
        .map_err(|error| format!("media_asset_error: failed to inspect cached asset: {error}"))?;
    if !metadata.is_file() || metadata.len() > MAX_CACHED_PREVIEW_BYTES {
        return Err("media_asset_error: cached preview is invalid or too large".to_owned());
    }
    let bytes = fs::read(&requested)
        .map_err(|error| format!("media_asset_error: failed to read cached preview: {error}"))?;
    if !bytes.starts_with(b"\x89PNG\r\n\x1a\n") {
        return Err("media_asset_error: cached preview is not a PNG file".to_owned());
    }
    Ok(Response::new(bytes))
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FrameAssetRef {
    pub path: String,
    pub format: String,
    pub width: u32,
    pub height: u32,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KernelCommand {
    pub id: String,
    pub b: Option<f64>,
    pub c: Option<f64>,
    pub taps: Option<u32>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MetricCommand {
    pub crop_left: Option<u32>,
    pub crop_right: Option<u32>,
    pub crop_top: Option<u32>,
    pub crop_bottom: Option<u32>,
    pub threshold: Option<f64>,
    pub p_norm: Option<u32>,
}

fn default_profile_id() -> String {
    "muf-d278cd3".to_owned()
}

fn default_endpoint_rule() -> String {
    "inclusive".to_owned()
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CandidateGridCommand {
    pub start: String,
    pub stop: String,
    pub step: String,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct WorkerAnalyzeRequest {
    pub request_id: String,
    pub mode: String,
    pub frame_asset: FrameAssetRef,
    pub axis_mode: String,
    /// Height mode: the single scan kernel. Kernel mode: omit.
    pub kernel: Option<KernelCommand>,
    /// Kernel mode: the ordered kernel list (fixed geometry from
    /// `candidates[0]`). Height mode: omit.
    pub kernels: Option<Vec<KernelCommand>>,
    pub candidates: Vec<String>,
    pub metric: MetricCommand,
    pub backend: String,
    pub worker_count: Option<u32>,
    #[serde(default = "default_profile_id")]
    pub profile_id: String,
    #[serde(default = "default_endpoint_rule")]
    pub endpoint_rule: String,
    pub base_height: Option<String>,
    pub base_width: Option<String>,
    pub grid: Option<CandidateGridCommand>,
}

fn validate_kernel_command(kernel: &KernelCommand) -> Result<(), String> {
    match kernel.id.as_str() {
        "bilinear" | "spline16" | "spline36" | "spline64" => {}
        "bicubic" => {
            if kernel.b.is_none() || kernel.c.is_none() {
                return Err("bad_request: bicubic requires explicit b and c".to_owned());
            }
            for (label, value) in [("b", kernel.b), ("c", kernel.c)] {
                if value.is_some_and(|value| !value.is_finite()) {
                    return Err(format!("bad_request: bicubic {label} must be finite"));
                }
            }
        }
        "lanczos" => {
            if kernel.taps.is_none() {
                return Err("bad_request: lanczos requires explicit taps".to_owned());
            }
            if kernel.taps.is_some_and(|taps| !(1..=15).contains(&taps)) {
                return Err("bad_request: lanczos taps must be within 1..=15".to_owned());
            }
        }
        other => return Err(format!("bad_request: unknown kernel id {other}")),
    }
    Ok(())
}

fn validate_analyze(request: &WorkerAnalyzeRequest) -> Result<(), String> {
    if request.request_id.trim().is_empty() {
        return Err("bad_request: requestId must not be empty".to_owned());
    }
    if !matches!(request.mode.as_str(), "height" | "kernel") {
        return Err(format!(
            "unsupported: mode must be height or kernel in worker protocol v1.1, got {}",
            request.mode
        ));
    }
    let asset = &request.frame_asset;
    if asset.path.trim().is_empty() {
        return Err("bad_request: frameAsset.path must not be empty".to_owned());
    }
    if asset.format != "f32le" {
        return Err(format!(
            "unsupported: frame asset format must be f32le in worker protocol v1, got {}",
            asset.format
        ));
    }
    if asset.width < 2 || asset.height < 2 || asset.width > MAX_FRAME_AXIS || asset.height > MAX_FRAME_AXIS {
        return Err(format!(
            "bad_request: frame asset dimensions must be within 2..={MAX_FRAME_AXIS}"
        ));
    }
    if !matches!(request.axis_mode.as_str(), "h_only" | "w_only" | "h_plus_w") {
        return Err(format!("bad_request: unknown axisMode {}", request.axis_mode));
    }
    if !matches!(
        request.profile_id.as_str(),
        "muf-d278cd3" | "getfnative-44c8d0f" | "modern"
    ) {
        return Err(format!("bad_request: unknown profileId {}", request.profile_id));
    }
    if !matches!(request.endpoint_rule.as_str(), "inclusive" | "exclusive_stop") {
        return Err(format!(
            "bad_request: unknown endpointRule {}",
            request.endpoint_rule
        ));
    }
    for (label, value) in [
        ("baseHeight", request.base_height.as_deref()),
        ("baseWidth", request.base_width.as_deref()),
    ] {
        if let Some(value) = value {
            let parsed = value.parse::<u32>().map_err(|_| {
                format!("bad_request: {label} must be a positive integer decimal")
            })?;
            if parsed == 0 || parsed > MAX_FRAME_AXIS {
                return Err(format!(
                    "bad_request: {label} must be within 1..={MAX_FRAME_AXIS}"
                ));
            }
        }
    }
    fn validate_kernel(kernel: &KernelCommand) -> Result<(), String> {
        validate_kernel_command(kernel)
    }
    if request.mode == "kernel" {
        if request.kernel.is_some() {
            return Err("bad_request: kernel mode takes kernels, not kernel".to_owned());
        }
        let kernels = request.kernels.as_ref().filter(|list| !list.is_empty());
        let Some(kernels) = kernels else {
            return Err("bad_request: kernel mode requires a non-empty kernels list".to_owned());
        };
        if kernels.len() > MAX_CANDIDATES {
            return Err(format!(
                "bad_request: kernels must contain 1..={MAX_CANDIDATES} entries"
            ));
        }
        for kernel in kernels {
            validate_kernel(kernel)?;
        }
        if request.candidates.len() != 1 {
            return Err(
                "bad_request: kernel mode takes exactly one candidate (the fixed axis value)"
                    .to_owned(),
            );
        }
    } else {
        if request.kernels.is_some() {
            return Err("bad_request: height mode takes kernel, not kernels".to_owned());
        }
        let Some(kernel) = request.kernel.as_ref() else {
            return Err("bad_request: height mode requires kernel".to_owned());
        };
        validate_kernel(kernel)?;
        if request.candidates.is_empty() || request.candidates.len() > MAX_CANDIDATES {
            return Err(format!(
                "bad_request: candidates must contain 1..={MAX_CANDIDATES} values"
            ));
        }
        if let Some(grid) = &request.grid {
            for (label, value) in [
                ("grid.start", grid.start.as_str()),
                ("grid.stop", grid.stop.as_str()),
                ("grid.step", grid.step.as_str()),
            ] {
                let parsed = value.parse::<f64>().map_err(|_| {
                    format!("bad_request: {label} must be a decimal")
                })?;
                if !parsed.is_finite() {
                    return Err(format!("bad_request: {label} must be finite"));
                }
            }
            if grid.step.parse::<f64>().unwrap_or(0.0) <= 0.0 {
                return Err("bad_request: grid.step must be positive".to_owned());
            }
        }
    }
    for candidate in &request.candidates {
        let Ok(value) = candidate.parse::<f64>() else {
            return Err(format!("bad_request: candidate {candidate:?} is not a decimal"));
        };
        if !value.is_finite() || value < 2.0 {
            return Err(format!(
                "bad_request: candidate {candidate:?} must be finite and >= 2"
            ));
        }
    }
    if request.metric.p_norm == Some(0) {
        return Err("bad_request: p_norm must be a positive integer".to_owned());
    }
    if request
        .metric
        .threshold
        .is_some_and(|threshold| !threshold.is_finite() || threshold < 0.0)
    {
        return Err("bad_request: threshold must be finite and non-negative".to_owned());
    }
    if !matches!(request.backend.as_str(), "cpu" | "cuda" | "vulkan" | "auto") {
        return Err(format!(
            "unsupported: backend must be one of cpu/cuda/vulkan/auto in worker protocol v1, got {}",
            request.backend
        ));
    }
    if request.backend == "cuda"
        && request.metric.p_norm.unwrap_or(1) > CUDA_MAXIMUM_P_NORM
    {
        return Err("unsupported: CUDA only supports p_norm in 1..4".to_owned());
    }
    if request.backend == "vulkan" && request.metric.p_norm.unwrap_or(1) != 1 {
        return Err("unsupported: Vulkan currently supports only p_norm=1".to_owned());
    }
    Ok(())
}

fn kernel_json(kernel: &KernelCommand) -> Value {
    let mut object = Map::new();
    object.insert("id".to_owned(), json!(kernel.id));
    if kernel.id == "bicubic" {
        if let Some(b) = kernel.b {
            object.insert("b".to_owned(), json!(b));
        }
        if let Some(c) = kernel.c {
            object.insert("c".to_owned(), json!(c));
        }
    }
    if kernel.id == "lanczos" {
        if let Some(taps) = kernel.taps {
            object.insert("taps".to_owned(), json!(taps));
        }
    }
    Value::Object(object)
}

fn analyze_command(request: &WorkerAnalyzeRequest) -> Value {
    let mut metric = Map::new();
    if let Some(value) = request.metric.crop_left {
        metric.insert("crop_left".to_owned(), json!(value));
    }
    if let Some(value) = request.metric.crop_right {
        metric.insert("crop_right".to_owned(), json!(value));
    }
    if let Some(value) = request.metric.crop_top {
        metric.insert("crop_top".to_owned(), json!(value));
    }
    if let Some(value) = request.metric.crop_bottom {
        metric.insert("crop_bottom".to_owned(), json!(value));
    }
    if let Some(value) = request.metric.threshold {
        metric.insert("threshold".to_owned(), json!(value));
    }
    if let Some(value) = request.metric.p_norm {
        metric.insert("p_norm".to_owned(), json!(value));
    }
    let mut command = json!({
        "protocol_version": PROTOCOL_VERSION,
        "type": "analyze",
        "request_id": request.request_id,
        "mode": request.mode,
        "frame_asset": {
            "path": request.frame_asset.path,
            "format": request.frame_asset.format,
            "width": request.frame_asset.width,
            "height": request.frame_asset.height,
        },
        "axis_mode": request.axis_mode,
        "profile_id": request.profile_id,
        "endpoint_rule": request.endpoint_rule,
        "base_height": request.base_height,
        "base_width": request.base_width,
        "metric": Value::Object(metric),
        "backend": request.backend,
    });
    if request.mode == "kernel" {
        // Kernel mode: the single fixed axis value travels as `candidate`
        // and the ordered kernel list as `kernels` (engine protocol v1.1).
        command["candidate"] = json!(request.candidates[0]);
        command["kernels"] = Value::Array(
            request
                .kernels
                .as_ref()
                .map(|kernels| kernels.iter().map(kernel_json).collect())
                .unwrap_or_default(),
        );
    } else {
        command["kernel"] = kernel_json(
            request.kernel.as_ref().expect("height mode requires kernel"),
        );
        command["candidates"] = json!(request.candidates);
        if let Some(grid) = &request.grid {
            command["grid"] = json!({
                "start": grid.start,
                "stop": grid.stop,
                "step": grid.step,
            });
        }
    }
    if let Some(worker_count) = request.worker_count {
        command["worker_count"] = json!(worker_count);
    }
    command
}

#[tauri::command]
pub fn engine_worker_analyze(
    state: State<'_, WorkerManager>,
    request: WorkerAnalyzeRequest,
) -> Result<Value, String> {
    validate_analyze(&request)?;
    let command = analyze_command(&request);
    let request_id = request.request_id.clone();
    state.with_live_session(|session| session.write_command(&command))?;
    Ok(json!({ "requestId": request_id, "queued": true }))
}

#[tauri::command]
pub fn engine_worker_cancel(state: State<'_, WorkerManager>, job_id: String) -> Result<(), String> {
    if job_id.trim().is_empty() {
        return Err("bad_request: jobId must not be empty".to_owned());
    }
    state.with_live_session(|session| {
        let command = json!({
            "protocol_version": PROTOCOL_VERSION,
            "type": "cancel",
            "request_id": session.next_request_id(),
            "job_id": job_id,
        });
        session.write_command(&command)
    })
}

#[tauri::command]
pub fn engine_worker_shutdown(state: State<'_, WorkerManager>) -> Result<(), String> {
    let session = state
        .session
        .lock()
        .map_err(|_| "worker_internal_error: session lock poisoned".to_owned())?
        .take();
    if let Some(mut session) = session {
        session.terminate();
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Verify streaming (worker protocol v1.1)
// ---------------------------------------------------------------------------

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct VerifyMediaBeginRequest {
    pub request_id: String,
    pub path: String,
    pub fingerprint: Option<String>,
    pub stream_index: u32,
    pub width: u32,
    pub height: u32,
    pub selection: String,
    pub every_n: Option<u64>,
    pub start_frame: Option<u64>,
    pub end_frame: Option<u64>,
    pub axis_mode: String,
    pub kernel: KernelCommand,
    pub candidate: String,
    pub metric: MetricCommand,
    pub backend: String,
    #[serde(default = "default_media_verify_concurrency")]
    pub concurrency: u32,
}

fn default_media_verify_concurrency() -> u32 {
    2
}

fn validate_verify_media_begin(request: &VerifyMediaBeginRequest) -> Result<(), String> {
    if request.request_id.trim().is_empty() || request.path.trim().is_empty() {
        return Err("bad_request: requestId and path must not be empty".to_owned());
    }
    if request.width < 2
        || request.height < 2
        || request.width > MAX_FRAME_AXIS
        || request.height > MAX_FRAME_AXIS
    {
        return Err(format!(
            "bad_request: verify geometry must be within 2..={MAX_FRAME_AXIS}"
        ));
    }
    if !matches!(request.axis_mode.as_str(), "h_only" | "w_only" | "h_plus_w") {
        return Err(format!("bad_request: unknown axisMode {}", request.axis_mode));
    }
    validate_kernel_command(&request.kernel)?;
    let candidate = request
        .candidate
        .parse::<f64>()
        .map_err(|_| "bad_request: candidate must be a decimal".to_owned())?;
    if !candidate.is_finite() || candidate < 2.0 {
        return Err("bad_request: candidate must be finite and >= 2".to_owned());
    }
    if !matches!(request.backend.as_str(), "cpu" | "cuda" | "vulkan" | "auto") {
        return Err(format!(
            "unsupported: media verify backend must be cpu/cuda/vulkan/auto, got {}",
            request.backend
        ));
    }
    if !(1..=8).contains(&request.concurrency) {
        return Err("bad_request: concurrency must be within 1..=8".to_owned());
    }
    if request.backend == "cuda" && request.metric.p_norm.unwrap_or(1) > CUDA_MAXIMUM_P_NORM {
        return Err("unsupported: CUDA verify only supports p_norm in 1..4".to_owned());
    }
    if request.backend == "vulkan" && request.metric.p_norm.unwrap_or(1) != 1 {
        return Err("unsupported: Vulkan verify currently supports only p_norm=1".to_owned());
    }
    match request.selection.as_str() {
        "all" | "decoded_i_picture" => {}
        "every_n" if request.every_n.is_some_and(|value| value >= 1) => {}
        "every_n" => {
            return Err("bad_request: every-N selection requires everyN >= 1".to_owned())
        }
        other => return Err(format!("bad_request: unknown selection rule {other}")),
    }
    if request
        .start_frame
        .zip(request.end_frame)
        .is_some_and(|(start, end)| start > end)
    {
        return Err("bad_request: range start must be <= end".to_owned());
    }
    Ok(())
}

fn metric_json(metric: &MetricCommand) -> Value {
    let mut result = Map::new();
    for (key, value) in [
        ("crop_left", metric.crop_left),
        ("crop_right", metric.crop_right),
        ("crop_top", metric.crop_top),
        ("crop_bottom", metric.crop_bottom),
    ] {
        if let Some(value) = value {
            result.insert(key.to_owned(), json!(value));
        }
    }
    if let Some(value) = metric.threshold {
        result.insert("threshold".to_owned(), json!(value));
    }
    if let Some(value) = metric.p_norm {
        result.insert("p_norm".to_owned(), json!(value));
    }
    Value::Object(result)
}

#[tauri::command]
pub fn engine_worker_verify_media_begin(
    app: AppHandle,
    state: State<'_, WorkerManager>,
    request: VerifyMediaBeginRequest,
) -> Result<Value, String> {
    validate_verify_media_begin(&request)?;
    let command = json!({
        "protocol_version": PROTOCOL_VERSION,
        "type": "verify_media_begin",
        "request_id": request.request_id,
        "media": {
            "path": request.path,
            "fingerprint": request.fingerprint,
            "stream_index": request.stream_index,
            "cache_directory": media_cache_directory(&app)?,
        },
        "geometry": { "width": request.width, "height": request.height },
        "scan_scope": {
            "selection": request.selection,
            "every_n": request.every_n,
            "start_frame": request.start_frame,
            "end_frame": request.end_frame,
        },
        "axis_mode": request.axis_mode,
        "kernel": kernel_json(&request.kernel),
        "candidate": request.candidate,
        "metric": metric_json(&request.metric),
        "backend": request.backend,
        "concurrency": request.concurrency,
    });
    let request_id = request.request_id.clone();
    state.with_live_session(|session| session.write_command(&command))?;
    Ok(json!({ "requestId": request_id, "queued": true }))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn analyze_request() -> WorkerAnalyzeRequest {
        serde_json::from_value(json!({
            "requestId": "req-1",
            "mode": "height",
            "frameAsset": {
                "path": "/tmp/frame.f32",
                "format": "f32le",
                "width": 320,
                "height": 240,
            },
            "axisMode": "h_only",
            "kernel": {"id": "bicubic", "b": 0.0, "c": 0.5},
            "candidates": ["230", "231.5"],
            "profileId": "getfnative-44c8d0f",
            "endpointRule": "exclusive_stop",
            "baseHeight": "241",
            "baseWidth": "321",
            "grid": {"start": "230", "stop": "233", "step": "1.5"},
            "metric": {"cropLeft": 10, "threshold": 0.015, "pNorm": 1},
            "backend": "cpu",
        }))
        .unwrap()
    }

    #[test]
    fn analyze_command_serializes_the_wire_shape() {
        let command = analyze_command(&analyze_request());
        assert_eq!(command["protocol_version"], json!(1));
        assert_eq!(command["type"], json!("analyze"));
        assert_eq!(command["request_id"], json!("req-1"));
        assert_eq!(command["frame_asset"]["format"], json!("f32le"));
        assert_eq!(command["kernel"], json!({"id": "bicubic", "b": 0.0, "c": 0.5}));
        assert_eq!(command["metric"]["p_norm"], json!(1));
        assert_eq!(command["profile_id"], json!("getfnative-44c8d0f"));
        assert_eq!(command["endpoint_rule"], json!("exclusive_stop"));
        assert_eq!(command["base_height"], json!("241"));
        assert_eq!(command["base_width"], json!("321"));
        assert_eq!(
            command["grid"],
            json!({"start": "230", "stop": "233", "step": "1.5"})
        );
        assert!(command.get("worker_count").is_none());
    }

    #[test]
    fn analyze_command_omits_irrelevant_kernel_parameters() {
        let mut request = analyze_request();
        request.kernel = Some(KernelCommand {
            id: "lanczos".to_owned(),
            b: Some(9.0),
            c: Some(9.0),
            taps: Some(3),
        });
        let command = analyze_command(&request);
        assert_eq!(command["kernel"], json!({"id": "lanczos", "taps": 3}));
    }

    #[test]
    fn kernel_mode_serializes_candidate_and_kernel_list() {
        let request: WorkerAnalyzeRequest = serde_json::from_value(json!({
            "requestId": "req-k1",
            "mode": "kernel",
            "frameAsset": {
                "path": "/tmp/frame.f32",
                "format": "f32le",
                "width": 320,
                "height": 240,
            },
            "axisMode": "h_only",
            "kernels": [
                {"id": "bicubic", "b": 0.0, "c": 0.5},
                {"id": "lanczos", "taps": 3},
            ],
            "candidates": ["200"],
            "metric": {"pNorm": 1},
            "backend": "cpu",
        }))
        .unwrap();
        validate_analyze(&request).unwrap();
        let command = analyze_command(&request);
        assert_eq!(command["mode"], json!("kernel"));
        assert_eq!(command["candidate"], json!("200"));
        assert_eq!(
            command["kernels"],
            json!([{"id": "bicubic", "b": 0.0, "c": 0.5}, {"id": "lanczos", "taps": 3}])
        );
        assert!(command.get("kernel").is_none());
        assert!(command.get("candidates").is_none());
    }

    #[test]
    fn kernel_mode_validation_rejects_bad_shapes() {
        let valid = json!({
            "requestId": "req-k2",
            "mode": "kernel",
            "frameAsset": {
                "path": "/tmp/frame.f32",
                "format": "f32le",
                "width": 320,
                "height": 240,
            },
            "axisMode": "h_only",
            "kernels": [{"id": "bicubic", "b": 0.0, "c": 0.5}],
            "candidates": ["200"],
            "metric": {"pNorm": 1},
            "backend": "cpu",
        });

        let mut value = valid.clone();
        value["candidates"] = json!(["200", "201"]);
        let request: WorkerAnalyzeRequest = serde_json::from_value(value).unwrap();
        assert!(validate_analyze(&request).is_err());

        let mut value = valid.clone();
        value["kernels"] = json!([]);
        let request: WorkerAnalyzeRequest = serde_json::from_value(value).unwrap();
        assert!(validate_analyze(&request).is_err());

        let mut value = valid.clone();
        value["kernel"] = json!({"id": "bicubic"});
        let request: WorkerAnalyzeRequest = serde_json::from_value(value).unwrap();
        assert!(validate_analyze(&request).is_err());

        let mut value = valid;
        value["kernels"] = json!([{"id": "lanczos", "taps": 16}]);
        let request: WorkerAnalyzeRequest = serde_json::from_value(value).unwrap();
        assert!(validate_analyze(&request).is_err());
    }

    #[test]
    fn analyze_validation_rejects_out_of_contract_shapes() {
        let mut request = analyze_request();
        request.mode = "width".to_owned();
        assert!(validate_analyze(&request).unwrap_err().contains("unsupported"));

        let mut request = analyze_request();
        request.frame_asset.format = "f64le".to_owned();
        assert!(validate_analyze(&request).is_err());

        let mut request = analyze_request();
        request.frame_asset.width = 1;
        assert!(validate_analyze(&request).is_err());

        let mut request = analyze_request();
        request.backend = "cuda".to_owned();
        assert!(validate_analyze(&request).is_ok());

        let mut request = analyze_request();
        request.backend = "auto".to_owned();
        assert!(validate_analyze(&request).is_ok());

        let mut request = analyze_request();
        request.backend = "vulkan".to_owned();
        assert!(validate_analyze(&request).is_ok());
        request.metric.p_norm = Some(2);
        assert!(validate_analyze(&request).is_err());

        let mut request = analyze_request();
        request.backend = "metal".to_owned();
        assert!(validate_analyze(&request).is_err());

        let mut request = analyze_request();
        request.candidates = vec!["1.5".to_owned()];
        assert!(validate_analyze(&request).is_err());

        let mut request = analyze_request();
        request.candidates = vec!["not-a-number".to_owned()];
        assert!(validate_analyze(&request).is_err());

        let mut request = analyze_request();
        request.metric.p_norm = Some(4);
        assert!(validate_analyze(&request).is_ok());
        request.backend = "cuda".to_owned();
        assert!(validate_analyze(&request).is_ok());
        request.metric.p_norm = Some(5);
        assert!(validate_analyze(&request).is_err());

        let mut request = analyze_request();
        if let Some(kernel) = request.kernel.as_mut() {
            kernel.taps = Some(16);
            kernel.id = "lanczos".to_owned();
        }
        assert!(validate_analyze(&request).is_err());
    }

    #[test]
    fn media_verify_concurrency_defaults_and_validates() {
        let value = json!({
            "requestId": "verify-media-1",
            "path": "/tmp/video.mkv",
            "fingerprint": null,
            "streamIndex": 0,
            "width": 320,
            "height": 240,
            "selection": "all",
            "everyN": null,
            "startFrame": null,
            "endFrame": null,
            "axisMode": "h_only",
            "kernel": {"id": "bicubic", "b": 0.0, "c": 0.5},
            "candidate": "200",
            "metric": {"pNorm": 1},
            "backend": "cpu"
        });
        let request: VerifyMediaBeginRequest =
            serde_json::from_value(value.clone()).unwrap();
        assert_eq!(request.concurrency, 2);
        assert!(validate_verify_media_begin(&request).is_ok());

        for concurrency in 1..=8 {
            let mut accepted = value.clone();
            accepted["concurrency"] = json!(concurrency);
            let request: VerifyMediaBeginRequest =
                serde_json::from_value(accepted).unwrap();
            assert!(validate_verify_media_begin(&request).is_ok());
        }
        for concurrency in [0, 9] {
            let mut rejected = value.clone();
            rejected["concurrency"] = json!(concurrency);
            let request: VerifyMediaBeginRequest =
                serde_json::from_value(rejected).unwrap();
            assert!(validate_verify_media_begin(&request).is_err());
        }
        for invalid in [json!(-1), json!(1.5), json!("two")] {
            let mut rejected = value.clone();
            rejected["concurrency"] = invalid;
            assert!(serde_json::from_value::<VerifyMediaBeginRequest>(rejected).is_err());
        }
    }

    #[test]
    fn dispatch_routes_pending_requests_and_forwards_job_events() {
        let pending: PendingMap = Arc::new(Mutex::new(HashMap::new()));
        let (events_tx, events_rx) = mpsc::channel();
        let sink: WorkerSink = Arc::new(move |output| {
            let _ = events_tx.send(output);
        });

        let (waiter_tx, waiter_rx) = mpsc::channel();
        pending
            .lock()
            .unwrap()
            .insert("req-hello".to_owned(), waiter_tx);

        dispatch_event(
            json!({"type": "hello_ok", "request_id": "req-hello"}),
            &pending,
            &sink,
        );
        assert_eq!(
            waiter_rx.recv_timeout(Duration::from_secs(1)).unwrap()["type"],
            json!("hello_ok")
        );
        assert!(events_rx.recv_timeout(Duration::from_millis(50)).is_err());

        dispatch_event(
            json!({"type": "progress", "request_id": "req-job", "job_id": "job-1"}),
            &pending,
            &sink,
        );
        match events_rx.recv_timeout(Duration::from_secs(1)).unwrap() {
            WorkerOutput::Event(value) => assert_eq!(value["job_id"], json!("job-1")),
            WorkerOutput::Exited { .. } => panic!("expected a job event"),
        }
    }

    /// End-to-end transport proof against a real engine binary:
    /// hello handshake, capabilities validation, a real height analyze with
    /// progress events and telemetry, cooperative cancel, and shutdown.
    #[test]
    #[ignore = "requires GETNATIVE_ENGINE_PATH pointing at a worker-capable engine build"]
    fn real_engine_worker_roundtrip() {
        let engine = PathBuf::from(
            std::env::var_os("GETNATIVE_ENGINE_PATH").expect("GETNATIVE_ENGINE_PATH must be set"),
        );
        let (tx, rx) = mpsc::channel();
        let sink: WorkerSink = Arc::new(move |output| {
            let _ = tx.send(output);
        });
        let mut session = spawn_session(&engine, sink, None).unwrap();

        let hello = session
            .roundtrip(json!({"protocol_version": 1, "type": "hello"}))
            .unwrap();
        assert_eq!(hello["type"], json!("hello_ok"));
        assert_eq!(hello["commands"]["analyze"], json!(true));

        let capabilities = session
            .roundtrip(json!({"protocol_version": 1, "type": "capabilities"}))
            .unwrap();
        let payload = capabilities["payload"].clone();
        validate_capabilities(&payload).unwrap();
        assert_eq!(payload["commands"]["analyze"], json!(true));

        // Deterministic frame asset, same recipe as the engine-side test.
        let frame_path = std::env::temp_dir().join(format!(
            "getnative_worker_transport_{}.f32",
            std::process::id()
        ));
        {
            let mut file = BufWriter::new(std::fs::File::create(&frame_path).unwrap());
            for y in 0..240_u32 {
                for x in 0..320_u32 {
                    let value = 0.5_f32 + 0.1 * ((x * 7 + y * 13) % 17) as f32 / 16.0;
                    file.write_all(&value.to_le_bytes()).unwrap();
                }
            }
            file.flush().unwrap();
        }

        let request = WorkerAnalyzeRequest {
            request_id: "gui-req-1".to_owned(),
            mode: "height".to_owned(),
            frame_asset: FrameAssetRef {
                path: frame_path.display().to_string(),
                format: "f32le".to_owned(),
                width: 320,
                height: 240,
            },
            axis_mode: "h_only".to_owned(),
            kernel: Some(KernelCommand {
                id: "bicubic".to_owned(),
                b: Some(0.0),
                c: Some(0.5),
                taps: None,
            }),
            kernels: None,
            candidates: (200..=220).map(|height| height.to_string()).collect(),
            metric: MetricCommand {
                crop_left: Some(10),
                crop_right: Some(10),
                crop_top: Some(10),
                crop_bottom: Some(10),
                threshold: Some(0.015),
                p_norm: Some(1),
            },
            backend: "cpu".to_owned(),
            worker_count: None,
            profile_id: default_profile_id(),
            endpoint_rule: default_endpoint_rule(),
            base_height: None,
            base_width: None,
            grid: None,
        };
        validate_analyze(&request).unwrap();
        session.write_command(&analyze_command(&request)).unwrap();

        let mut saw_accepted = false;
        let mut saw_progress = false;
        let mut result = None;
        let deadline = std::time::Instant::now() + Duration::from_secs(60);
        while result.is_none() && std::time::Instant::now() < deadline {
            match rx.recv_timeout(Duration::from_secs(10)) {
                Ok(WorkerOutput::Event(value)) => match value["type"].as_str() {
                    Some("accepted") => {
                        saw_accepted = true;
                        assert_eq!(value["request_id"], json!("gui-req-1"));
                        assert!(value["job_id"].as_str().is_some_and(|id| id.starts_with("job-")));
                    }
                    Some("progress") => saw_progress = true,
                    Some("result") => result = Some(value),
                    Some("error") => panic!("engine reported an error: {value}"),
                    _ => {}
                },
                Ok(WorkerOutput::Exited { stderr_tail }) => {
                    panic!("worker exited mid-job: {stderr_tail:?}")
                }
                Err(error) => panic!("timed out waiting for job events: {error}"),
            }
        }
        assert!(saw_accepted && saw_progress);
        let result = result.expect("the analyze job must produce a result");
        let payload = &result["payload"];
        assert_eq!(payload["mode"], json!("height"));
        assert_eq!(payload["candidates"].as_array().unwrap().len(), 21);
        assert!(
            payload["telemetry"]["plan_build_count"].as_u64().unwrap() >= 1,
            "telemetry must report real plan builds: {payload}"
        );

        // Session cache: the same candidates again must hit warm plans.
        let mut second = analyze_command(&request);
        second["request_id"] = json!("gui-req-2");
        session.write_command(&second).unwrap();
        loop {
            match rx.recv_timeout(Duration::from_secs(30)) {
                Ok(WorkerOutput::Event(value)) if value["type"] == "result" => {
                    let hits = value["payload"]["telemetry"]["plan_cache_hits"]
                        .as_u64()
                        .unwrap();
                    assert!(hits >= 1, "second job must hit the session plan cache: {value}");
                    break;
                }
                Ok(WorkerOutput::Event(_)) => {}
                Ok(WorkerOutput::Exited { stderr_tail }) => {
                    panic!("worker exited mid-job: {stderr_tail:?}")
                }
                Err(error) => panic!("timed out waiting for the second result: {error}"),
            }
        }

        session.terminate();
        let _ = std::fs::remove_file(frame_path);
    }

    /// Full-loop proof: a real still image goes through the media frame-asset
    /// exporter and the resulting f32le file feeds a worker analyze job.
    #[test]
    #[ignore = "requires GETNATIVE_ENGINE_PATH pointing at a worker-capable engine build"]
    fn media_asset_to_worker_analyze_roundtrip() {
        let engine = PathBuf::from(
            std::env::var_os("GETNATIVE_ENGINE_PATH").expect("GETNATIVE_ENGINE_PATH must be set"),
        );
        let unique = std::process::id();
        let png_path = std::env::temp_dir().join(format!("getnative_transport_src_{unique}.png"));
        let cache_dir = std::env::temp_dir().join(format!("getnative_transport_cache_{unique}"));
        let gradient = image::ImageBuffer::from_fn(320, 240, |x, y| {
            let level = ((x * 7 + y * 13) % 256) as u8;
            image::Rgb([level, level, level])
        });
        image::DynamicImage::ImageRgb8(gradient)
            .save(&png_path)
            .unwrap();

        let asset = crate::media::frame_asset_still(
            &png_path,
            &crate::media::MediaFrameAssetRequest {
                path: png_path.display().to_string(),
                fingerprint: None,
                stream_index: None,
                frame_index: None,
                width: Some(320),
                height: Some(240),
            },
            &cache_dir,
        )
        .unwrap();
        assert_eq!(asset.format, "f32le");
        assert_eq!((asset.width, asset.height), (320, 240));

        let (tx, rx) = mpsc::channel();
        let sink: WorkerSink = Arc::new(move |output| {
            let _ = tx.send(output);
        });
        let mut session = spawn_session(&engine, sink, None).unwrap();
        session
            .roundtrip(json!({"protocol_version": 1, "type": "hello"}))
            .unwrap();

        let request = WorkerAnalyzeRequest {
            request_id: "gui-media-1".to_owned(),
            mode: "height".to_owned(),
            frame_asset: FrameAssetRef {
                path: asset.path.clone(),
                format: asset.format.clone(),
                width: asset.width,
                height: asset.height,
            },
            axis_mode: "h_only".to_owned(),
            kernel: Some(KernelCommand {
                id: "bicubic".to_owned(),
                b: Some(0.0),
                c: Some(0.5),
                taps: None,
            }),
            kernels: None,
            candidates: vec!["220".to_owned(), "221".to_owned(), "222".to_owned()],
            metric: MetricCommand {
                crop_left: Some(10),
                crop_right: Some(10),
                crop_top: Some(10),
                crop_bottom: Some(10),
                threshold: Some(0.015),
                p_norm: Some(1),
            },
            backend: "cpu".to_owned(),
            worker_count: None,
            profile_id: default_profile_id(),
            endpoint_rule: default_endpoint_rule(),
            base_height: None,
            base_width: None,
            grid: None,
        };
        session.write_command(&analyze_command(&request)).unwrap();
        loop {
            match rx.recv_timeout(Duration::from_secs(30)) {
                Ok(WorkerOutput::Event(value)) if value["type"] == "result" => {
                    let candidates = value["payload"]["candidates"].as_array().unwrap();
                    assert_eq!(candidates.len(), 3);
                    assert!(
                        candidates
                            .iter()
                            .all(|candidate| candidate["error"].as_f64().is_some_and(|e| e >= 0.0)),
                        "every candidate must carry a real metric: {value}"
                    );
                    break;
                }
                Ok(WorkerOutput::Event(value)) if value["type"] == "error" => {
                    panic!("engine reported an error: {value}")
                }
                Ok(WorkerOutput::Event(_)) => {}
                Ok(WorkerOutput::Exited { stderr_tail }) => {
                    panic!("worker exited mid-job: {stderr_tail:?}")
                }
                Err(error) => panic!("timed out waiting for the media-backed result: {error}"),
            }
        }

        session.terminate();
        let _ = std::fs::remove_file(png_path);
        let _ = std::fs::remove_dir_all(cache_dir);
    }

    #[test]
    #[ignore = "requires GETNATIVE_ENGINE_PATH and GETNATIVE_MUF_FIXTURE"]
    fn muf_reference_image_conformance() {
        let engine = PathBuf::from(
            std::env::var_os("GETNATIVE_ENGINE_PATH").expect("GETNATIVE_ENGINE_PATH must be set"),
        );
        let fixture = PathBuf::from(
            std::env::var_os("GETNATIVE_MUF_FIXTURE").expect("GETNATIVE_MUF_FIXTURE must be set"),
        );
        let cache_dir = std::env::temp_dir().join(format!(
            "getnative_muf_conformance_{}_{}",
            std::process::id(),
            uuid::Uuid::new_v4()
        ));
        let asset = crate::media::frame_asset_still(
            &fixture,
            &crate::media::MediaFrameAssetRequest {
                path: fixture.display().to_string(),
                fingerprint: None,
                stream_index: None,
                frame_index: None,
                width: Some(1920),
                height: Some(1080),
            },
            &cache_dir,
        )
        .unwrap();

        let (tx, rx) = mpsc::channel();
        let sink: WorkerSink = Arc::new(move |output| {
            let _ = tx.send(output);
        });
        let mut session = spawn_session(&engine, sink, None).unwrap();
        session
            .roundtrip(json!({"protocol_version": 1, "type": "hello"}))
            .unwrap();
        let capabilities = session
            .roundtrip(json!({"protocol_version": 1, "type": "capabilities"}))
            .unwrap();
        let cuda_available = capabilities["payload"]["backends"]
            .as_array()
            .is_some_and(|backends| {
                backends.iter().any(|backend| {
                    backend["id"] == "cuda"
                        && backend["compiled"] == true
                        && backend["device_available"] == true
                })
            });
        let backends: &[&str] = if cuda_available {
            &["cpu", "cuda"]
        } else {
            &["cpu"]
        };

        for backend in backends {
            for (axis_mode, expected) in [
                ("h_only", 1.695259983e-7_f64),
                ("h_plus_w", 2.251854147e-6_f64),
            ] {
                let request: WorkerAnalyzeRequest = serde_json::from_value(json!({
                    "requestId": format!("muf-conformance-{backend}-{axis_mode}"),
                    "mode": "height",
                    "frameAsset": {
                        "path": asset.path,
                        "format": "f32le",
                        "width": asset.width,
                        "height": asset.height,
                    },
                    "axisMode": axis_mode,
                    "kernel": {"id": "bicubic", "b": 0.0, "c": 0.5},
                    "candidates": ["810"],
                    "profileId": "muf-d278cd3",
                    "endpointRule": "inclusive",
                    "baseHeight": null,
                    "baseWidth": null,
                    "grid": {"start": "810", "stop": "810", "step": "1"},
                    "metric": {
                        "cropLeft": 5, "cropRight": 5, "cropTop": 5, "cropBottom": 5,
                        "threshold": 0.015, "pNorm": 1
                    },
                    "backend": backend,
                }))
                .unwrap();
                validate_analyze(&request).unwrap();
                session.write_command(&analyze_command(&request)).unwrap();

                let actual = loop {
                    match rx.recv_timeout(Duration::from_secs(60)) {
                        Ok(WorkerOutput::Event(value)) if value["type"] == "result" => {
                            break value["payload"]["candidates"][0]["error"]
                                .as_f64()
                                .expect("candidate error must be numeric");
                        }
                        Ok(WorkerOutput::Event(value)) if value["type"] == "error" => {
                            panic!("engine reported an error: {value}")
                        }
                        Ok(WorkerOutput::Event(_)) => {}
                        Ok(WorkerOutput::Exited { stderr_tail }) => {
                            panic!("worker exited mid-job: {stderr_tail:?}")
                        }
                        Err(error) => {
                            panic!("timed out waiting for conformance result: {error}")
                        }
                    }
                };
                let relative_error = (actual - expected).abs() / expected.abs();
                println!(
                    "MUF {backend} {axis_mode}: actual={actual:.12e}, expected={expected:.12e}, relative={relative_error:.3e}"
                );
                assert!(
                    relative_error <= 1e-5,
                    "{backend} {axis_mode}: actual={actual:.12e}, expected={expected:.12e}, relative={relative_error:.3e}"
                );
            }
        }

        session.terminate();
        let _ = std::fs::remove_dir_all(cache_dir);
    }

    /// A fake worker that answers hello and then exits: the sink must observe
    /// the exit and pending roundtrips must fail instead of hanging.
    #[cfg(unix)]
    #[test]
    fn dead_worker_surfaces_exit_and_fails_pending_commands() {
        let script = std::env::temp_dir().join(format!(
            "getnative_fake_worker_{}.sh",
            std::process::id()
        ));
        std::fs::write(
            &script,
            "#!/bin/sh\nread line\necho '{\"protocol_version\":1,\"type\":\"hello_ok\",\"request_id\":\"tauri-internal-1\",\"timestamp_ms\":0,\"engine_version\":\"fake\",\"commands\":{\"analyze\":true,\"cancel\":true}}'\n",
        )
        .unwrap();
        std::fs::set_permissions(&script, {
            use std::os::unix::fs::PermissionsExt;
            std::fs::Permissions::from_mode(0o755)
        })
        .unwrap();

        let (tx, rx) = mpsc::channel();
        let sink: WorkerSink = Arc::new(move |output| {
            let _ = tx.send(output);
        });
        // spawn_session appends the "worker" argument; the script ignores it.
        let mut session = spawn_session(&script, sink, None).unwrap();
        let hello = session
            .roundtrip(json!({"protocol_version": 1, "type": "hello"}))
            .unwrap();
        assert_eq!(hello["type"], json!("hello_ok"));

        match rx.recv_timeout(Duration::from_secs(5)).unwrap() {
            WorkerOutput::Exited { .. } => {}
            WorkerOutput::Event(value) => panic!("expected an exit, got {value}"),
        }
        let result = session.roundtrip(json!({"protocol_version": 1, "type": "capabilities"}));
        assert!(result.is_err(), "a dead worker must fail roundtrips");
        session.terminate();
        let _ = std::fs::remove_file(script);
    }

    #[cfg(unix)]
    #[test]
    fn malformed_worker_output_becomes_a_protocol_error_event() {
        let script = std::env::temp_dir().join(format!(
            "getnative_fake_worker_bad_{}.sh",
            std::process::id()
        ));
        std::fs::write(&script, "#!/bin/sh\necho 'not json'\nsleep 60\n").unwrap();
        std::fs::set_permissions(&script, {
            use std::os::unix::fs::PermissionsExt;
            std::fs::Permissions::from_mode(0o755)
        })
        .unwrap();

        let (tx, rx) = mpsc::channel();
        let sink: WorkerSink = Arc::new(move |output| {
            let _ = tx.send(output);
        });
        let mut session = spawn_session(&script, sink, None).unwrap();
        match rx.recv_timeout(Duration::from_secs(5)).unwrap() {
            WorkerOutput::Event(value) => {
                assert_eq!(value["type"], json!("error"));
                assert_eq!(value["code"], json!("protocol_error"));
            }
            WorkerOutput::Exited { .. } => panic!("expected a protocol error event"),
        }
        session.terminate();
        let _ = std::fs::remove_file(script);
    }
}
