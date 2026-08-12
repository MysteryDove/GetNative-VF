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
//!
//! Layout: `session` is the pure transport (process + framing), `protocol`
//! owns request shapes/validation/serialization, `assets` owns the media
//! cache and asset path safety. This module keeps the Tauri-facing pieces:
//! `WorkerManager` and the command handlers.

pub mod protocol;
pub(crate) mod assets;
pub(crate) mod session;

pub(crate) use assets::migrate_legacy_media_cache;

use crate::engine::{find_engine, validate_capabilities};
use assets::media_cache_directory;
use protocol::{
    analyze_command, validate_analyze, validate_verify_media_begin, verify_media_begin_command,
    WorkerAnalyzeRequest, VerifyMediaBeginRequest,
};
use serde_json::{json, Value};
use session::{spawn_session, WorkerOutput, WorkerSession, WorkerSink};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use tauri::{ipc::Response, AppHandle, Emitter, State};

pub(crate) const PROTOCOL_VERSION: u32 = 1;

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

#[derive(Clone, Default)]
pub struct WorkerManager {
    session: Arc<Mutex<Option<WorkerSession>>>,
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
pub async fn engine_worker_start(
    app: AppHandle,
    state: State<'_, WorkerManager>,
) -> Result<Value, String> {
    // Clone the Arc-backed manager: `State` lifetimes cannot cross spawn_blocking.
    let manager = state.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let mut guard = manager
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
    })
    .await
    .map_err(|error| format!("worker_task_error: {error}"))?
}

#[tauri::command]
pub async fn engine_worker_capabilities(state: State<'_, WorkerManager>) -> Result<Value, String> {
    let manager = state.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        manager.with_live_session(|session| {
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
    })
    .await
    .map_err(|error| format!("worker_task_error: {error}"))?
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
pub async fn engine_worker_media_read_asset(
    app: AppHandle,
    path: String,
) -> Result<Response, String> {
    tauri::async_runtime::spawn_blocking(move || {
        assets::read_cached_preview(&app, &path).map(Response::new)
    })
    .await
    .map_err(|error| format!("media_asset_task_error: {error}"))?
}

#[tauri::command]
pub fn engine_worker_analyze(
    state: State<'_, WorkerManager>,
    request: WorkerAnalyzeRequest,
) -> Result<Value, String> {
    validate_analyze(&request)?;
    let command = analyze_command(&request)?;
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
pub async fn engine_worker_shutdown(state: State<'_, WorkerManager>) -> Result<(), String> {
    let manager = state.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let session = manager
            .session
            .lock()
            .map_err(|_| "worker_internal_error: session lock poisoned".to_owned())?
            .take();
        if let Some(mut session) = session {
            session.terminate();
        }
        Ok(())
    })
    .await
    .map_err(|error| format!("worker_task_error: {error}"))?
}

#[tauri::command]
pub fn engine_worker_verify_media_begin(
    app: AppHandle,
    state: State<'_, WorkerManager>,
    request: VerifyMediaBeginRequest,
) -> Result<Value, String> {
    validate_verify_media_begin(&request)?;
    let command = verify_media_begin_command(&request, &media_cache_directory(&app)?);
    let request_id = request.request_id.clone();
    state.with_live_session(|session| session.write_command(&command))?;
    Ok(json!({ "requestId": request_id, "queued": true }))
}
