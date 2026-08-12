//! Pure transport for the resident engine worker: process lifecycle, JSONL
//! framing on stdin/stdout, stderr tail capture, and request/response
//! correlation. Tauri-independent — the tests below drive it against fake
//! worker scripts and (env-gated) a real engine binary.

use serde_json::{json, Value};
use std::collections::{HashMap, VecDeque};
use std::io::{BufRead, BufReader, BufWriter, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::Duration;

use super::PROTOCOL_VERSION;

const STDERR_TAIL_LINES: usize = 50;
const RESPONSE_TIMEOUT: Duration = Duration::from_secs(30);
const SHUTDOWN_TIMEOUT: Duration = Duration::from_secs(5);

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

pub(crate) struct WorkerSession {
    child: Child,
    stdin: BufWriter<ChildStdin>,
    pending: PendingMap,
    next_internal_request: AtomicU64,
    pub(crate) engine_path: PathBuf,
    pub(crate) hello: Value,
}

impl WorkerSession {
    pub(crate) fn alive(&mut self) -> bool {
        matches!(self.child.try_wait(), Ok(None))
    }

    pub(crate) fn next_request_id(&self) -> String {
        format!("tauri-internal-{}", self.next_internal_request.fetch_add(1, Ordering::Relaxed))
    }

    pub(crate) fn write_command(&mut self, command: &Value) -> Result<(), String> {
        let line = serde_json::to_string(command)
            .map_err(|error| format!("worker_protocol_error: failed to encode command: {error}"))?;
        self.stdin
            .write_all(line.as_bytes())
            .and_then(|()| self.stdin.write_all(b"\n"))
            .and_then(|()| self.stdin.flush())
            .map_err(|error| format!("worker_io_error: failed to send command to the engine worker: {error}"))
    }

    /// Send a command and wait for the event that carries its request id.
    pub(crate) fn roundtrip(&mut self, mut command: Value) -> Result<Value, String> {
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

    pub(crate) fn terminate(&mut self) {
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

pub(crate) fn spawn_session(
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::engine::validate_capabilities;
    use crate::worker::protocol::{
        analyze_command, default_endpoint_rule, default_profile_id, validate_analyze,
        FrameAssetRef, KernelCommand, MetricCommand, WorkerAnalyzeRequest,
    };

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
        session
            .write_command(&analyze_command(&request).unwrap())
            .unwrap();

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
        let mut second = analyze_command(&request).unwrap();
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
        session
            .write_command(&analyze_command(&request).unwrap())
            .unwrap();
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
                session
                    .write_command(&analyze_command(&request).unwrap())
                    .unwrap();

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
