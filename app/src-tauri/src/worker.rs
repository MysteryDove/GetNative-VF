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
use std::io::{BufRead, BufReader, BufWriter, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::Duration;
use tauri::{AppHandle, Emitter, State};

const PROTOCOL_VERSION: u32 = 1;
const STDERR_TAIL_LINES: usize = 50;
const RESPONSE_TIMEOUT: Duration = Duration::from_secs(30);
const SHUTDOWN_TIMEOUT: Duration = Duration::from_secs(5);
const MAX_FRAME_AXIS: u32 = 65_536;
const MAX_CANDIDATES: usize = 100_000;

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

fn spawn_session(engine_path: &Path, sink: WorkerSink) -> Result<WorkerSession, String> {
    let mut child = Command::new(engine_path)
        .arg("worker")
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
    let mut session = spawn_session(&path, tauri_sink(&app))?;
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
}

fn validate_kernel_command(kernel: &KernelCommand) -> Result<(), String> {
    match kernel.id.as_str() {
        "bilinear" | "spline16" | "spline36" | "spline64" => {}
        "bicubic" => {
            for (label, value) in [("b", kernel.b), ("c", kernel.c)] {
                if value.is_some_and(|value| !value.is_finite()) {
                    return Err(format!("bad_request: bicubic {label} must be finite"));
                }
            }
        }
        "lanczos" => {
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
    if request.metric.p_norm.is_some_and(|p| p != 1) {
        return Err("unsupported: only p_norm=1 is available in worker protocol v1".to_owned());
    }
    if request
        .metric
        .threshold
        .is_some_and(|threshold| !threshold.is_finite() || threshold < 0.0)
    {
        return Err("bad_request: threshold must be finite and non-negative".to_owned());
    }
    if !matches!(request.backend.as_str(), "cpu" | "cuda" | "auto") {
        return Err(format!(
            "unsupported: backend must be one of cpu/cuda/auto in worker protocol v1, got {}",
            request.backend
        ));
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
pub struct VerifyBeginRequest {
    pub request_id: String,
    /// Frame dimensions every streamed asset must match.
    pub width: u32,
    pub height: u32,
    pub axis_mode: String,
    pub kernel: KernelCommand,
    /// Locked recipe's fixed axis value (decimal string).
    pub candidate: String,
    pub metric: MetricCommand,
    pub backend: String,
    pub worker_count: Option<u32>,
    pub expected_frames: Option<u64>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct VerifyFrameRequest {
    pub request_id: String,
    pub job_id: String,
    pub seq: u64,
    pub frame_asset: FrameAssetRef,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct VerifyEndRequest {
    pub request_id: String,
    pub job_id: String,
    pub total: u64,
}

fn validate_frame_asset_ref(asset: &FrameAssetRef) -> Result<(), String> {
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
    Ok(())
}

fn validate_verify_begin(request: &VerifyBeginRequest) -> Result<(), String> {
    if request.request_id.trim().is_empty() {
        return Err("bad_request: requestId must not be empty".to_owned());
    }
    if request.width < 2 || request.height < 2 || request.width > MAX_FRAME_AXIS || request.height > MAX_FRAME_AXIS {
        return Err(format!(
            "bad_request: verify geometry must be within 2..={MAX_FRAME_AXIS}"
        ));
    }
    if !matches!(request.axis_mode.as_str(), "h_only" | "w_only" | "h_plus_w") {
        return Err(format!("bad_request: unknown axisMode {}", request.axis_mode));
    }
    validate_kernel_command(&request.kernel)?;
    let Ok(value) = request.candidate.parse::<f64>() else {
        return Err(format!(
            "bad_request: candidate {:?} is not a decimal",
            request.candidate
        ));
    };
    if !value.is_finite() || value < 2.0 {
        return Err("bad_request: candidate must be finite and >= 2".to_owned());
    }
    if !matches!(request.backend.as_str(), "cpu" | "auto") {
        return Err(format!(
            "unsupported: verify is CPU-only in worker protocol v1.1, got {}",
            request.backend
        ));
    }
    if let Some(expected) = request.expected_frames {
        if !(1..=1_000_000).contains(&expected) {
            return Err("bad_request: expectedFrames must be within 1..=1000000".to_owned());
        }
    }
    Ok(())
}

#[tauri::command]
pub fn engine_worker_verify_begin(
    state: State<'_, WorkerManager>,
    request: VerifyBeginRequest,
) -> Result<Value, String> {
    validate_verify_begin(&request)?;
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
        "type": "verify_begin",
        "request_id": request.request_id,
        "geometry": { "width": request.width, "height": request.height },
        "axis_mode": request.axis_mode,
        "kernel": kernel_json(&request.kernel),
        "candidate": request.candidate,
        "metric": Value::Object(metric),
        "backend": request.backend,
    });
    if let Some(worker_count) = request.worker_count {
        command["worker_count"] = json!(worker_count);
    }
    if let Some(expected) = request.expected_frames {
        command["expected_frames"] = json!(expected);
    }
    let request_id = request.request_id.clone();
    state.with_live_session(|session| session.write_command(&command))?;
    Ok(json!({ "requestId": request_id, "queued": true }))
}

#[tauri::command]
pub fn engine_worker_verify_frame(
    state: State<'_, WorkerManager>,
    request: VerifyFrameRequest,
) -> Result<Value, String> {
    if request.job_id.trim().is_empty() {
        return Err("bad_request: jobId must not be empty".to_owned());
    }
    validate_frame_asset_ref(&request.frame_asset)?;
    let command = json!({
        "protocol_version": PROTOCOL_VERSION,
        "type": "verify_frame",
        "request_id": request.request_id,
        "job_id": request.job_id,
        "seq": request.seq,
        "frame_asset": {
            "path": request.frame_asset.path,
            "format": request.frame_asset.format,
            "width": request.frame_asset.width,
            "height": request.frame_asset.height,
        },
    });
    let request_id = request.request_id.clone();
    state.with_live_session(|session| session.write_command(&command))?;
    Ok(json!({ "requestId": request_id, "queued": true }))
}

#[tauri::command]
pub fn engine_worker_verify_end(
    state: State<'_, WorkerManager>,
    request: VerifyEndRequest,
) -> Result<Value, String> {
    if request.job_id.trim().is_empty() {
        return Err("bad_request: jobId must not be empty".to_owned());
    }
    let command = json!({
        "protocol_version": PROTOCOL_VERSION,
        "type": "verify_end",
        "request_id": request.request_id,
        "job_id": request.job_id,
        "total": request.total,
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
        request.backend = "metal".to_owned();
        assert!(validate_analyze(&request).is_err());

        let mut request = analyze_request();
        request.candidates = vec!["1.5".to_owned()];
        assert!(validate_analyze(&request).is_err());

        let mut request = analyze_request();
        request.candidates = vec!["not-a-number".to_owned()];
        assert!(validate_analyze(&request).is_err());

        let mut request = analyze_request();
        request.metric.p_norm = Some(2);
        assert!(validate_analyze(&request).is_err());

        let mut request = analyze_request();
        if let Some(kernel) = request.kernel.as_mut() {
            kernel.taps = Some(16);
            kernel.id = "lanczos".to_owned();
        }
        assert!(validate_analyze(&request).is_err());
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
        let mut session = spawn_session(&engine, sink).unwrap();

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
        let mut session = spawn_session(&engine, sink).unwrap();
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
        let mut session = spawn_session(&script, sink).unwrap();
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
        let mut session = spawn_session(&script, sink).unwrap();
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
