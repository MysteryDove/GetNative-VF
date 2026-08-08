//! Streaming frame-asset producer for whole-video verification (GUI-5).
//!
//! One FFmpeg process decodes the selected scope sequentially to grayf32le;
//! each produced frame lands as an atomic f32le asset and is announced with a
//! `media-verify-asset` event. The consumer (engine worker, via the frontend
//! orchestrator) acknowledges consumed seqs through `media_verify_stream_ack`,
//! which deletes the file and frees a production slot, so on-disk assets stay
//! bounded by `in_flight`. `media_verify_stream_abort` cancels cooperatively.

use crate::media::{
    build_frame_index, frame_index_cache_path, quick_fingerprint, resolve_media_tool,
    validate_expected_fingerprint, validated_media_path, FrameIdentity,
};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs::{self, File};
use std::io::{BufWriter, Read, Write};
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use tauri::{AppHandle, Emitter, Manager};
use uuid::Uuid;

const DEFAULT_IN_FLIGHT: usize = 16;
const MAX_IN_FLIGHT: usize = 64;

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct VerifyStreamRequest {
    pub path: String,
    pub fingerprint: Option<String>,
    pub stream_index: u32,
    pub selection: String,
    pub every_n: Option<u64>,
    pub start_frame: Option<u64>,
    pub end_frame: Option<u64>,
    pub width: u32,
    pub height: u32,
    pub in_flight: Option<usize>,
}

#[derive(Debug, Clone, Serialize)]
pub struct VerifyStreamInfo {
    pub ticket: String,
    pub total: u64,
    pub width: u32,
    pub height: u32,
}

struct StreamShared {
    unacked: Mutex<std::collections::HashSet<u64>>,
    slot_available: Condvar,
    cancelled: AtomicBool,
}

#[derive(Default)]
pub struct VerifyStreamManager {
    streams: Mutex<HashMap<String, Arc<StreamShared>>>,
}

#[tauri::command]
pub fn media_verify_stream_start(
    app: AppHandle,
    manager: tauri::State<'_, VerifyStreamManager>,
    request: VerifyStreamRequest,
) -> Result<VerifyStreamInfo, String> {
    let path = validated_media_path(&request.path)?;
    validate_expected_fingerprint(&path, request.fingerprint.as_deref())?;
    let ffprobe = resolve_media_tool(&app, "ffprobe", "GETNATIVE_FFPROBE_PATH")
        .ok_or_else(|| "video_backend_unavailable: bundled FFprobe is not available".to_owned())?;
    let ffmpeg = resolve_media_tool(&app, "ffmpeg", "GETNATIVE_FFMPEG_PATH")
        .ok_or_else(|| "video_backend_unavailable: bundled FFmpeg is not available".to_owned())?;
    if request.width < 2 || request.height < 2 {
        return Err("bad_request: verify stream requires probed dimensions".to_owned());
    }
    let frame_bytes =
        usize::try_from(crate::media::frame_asset_expected_bytes(request.width, request.height)?)
            .map_err(|_| "bad_request: frame dimensions overflow this platform".to_owned())?;

    let metadata = fs::metadata(&path)
        .map_err(|error| format!("media_read_error: failed to read media metadata: {error}"))?;
    let fingerprint = quick_fingerprint(&path, metadata.len())?;
    let index_path = frame_index_cache_path(&app, &fingerprint, request.stream_index)?;
    if !index_path.is_file() {
        build_frame_index(&ffprobe.path, &path, request.stream_index, &index_path)?;
    }
    let selected = select_frames(&index_path, &request)?;
    if selected.is_empty() {
        return Err("bad_request: the scan scope selects zero frames".to_owned());
    }

    let ticket = format!("vstream_{}", Uuid::new_v4());
    let asset_dir = app
        .path()
        .app_cache_dir()
        .map_err(|error| format!("frame_asset_cache_error: {error}"))?
        .join("frame-assets")
        .join(&ticket);
    fs::create_dir_all(&asset_dir).map_err(|error| format!("frame_asset_cache_error: {error}"))?;

    let in_flight = request
        .in_flight
        .unwrap_or(DEFAULT_IN_FLIGHT)
        .clamp(1, MAX_IN_FLIGHT);
    let shared = Arc::new(StreamShared {
        unacked: Mutex::new(std::collections::HashSet::new()),
        slot_available: Condvar::new(),
        cancelled: AtomicBool::new(false),
    });
    manager
        .streams
        .lock()
        .map_err(|_| "worker_internal_error: stream registry lock poisoned".to_owned())?
        .insert(ticket.clone(), Arc::clone(&shared));

    let info = VerifyStreamInfo {
        ticket: ticket.clone(),
        total: selected.len() as u64,
        width: request.width,
        height: request.height,
    };

    let producer_app = app.clone();
    let producer_ticket = ticket.clone();
    std::thread::spawn(move || {
        let outcome = run_producer(ProducerParams {
            app: producer_app.clone(),
            ticket: producer_ticket.clone(),
            ffmpeg: ffmpeg.path,
            media_path: path,
            stream_index: request.stream_index,
            selected,
            width: request.width,
            height: request.height,
            frame_bytes,
            asset_dir,
            in_flight,
            shared: Arc::clone(&shared),
        });
        if let Err(message) = &outcome {
            let _ = producer_app.emit(
                "media-verify-stream-error",
                serde_json::json!({ "ticket": producer_ticket, "message": message }),
            );
        }
        let _ = producer_app.emit(
            "media-verify-stream-done",
            serde_json::json!({
                "ticket": producer_ticket,
                "produced": outcome.as_ref().map(|produced| *produced).unwrap_or(0),
            }),
        );
        // Producer finished: drop the registry entry; late acks are no-ops.
        shared.cancelled.store(true, Ordering::SeqCst);
    });

    Ok(info)
}

#[tauri::command]
pub fn media_verify_stream_ack(
    app: AppHandle,
    manager: tauri::State<'_, VerifyStreamManager>,
    ticket: String,
    seq: u64,
) -> Result<(), String> {
    let shared = {
        manager
            .streams
            .lock()
            .map_err(|_| "worker_internal_error: stream registry lock poisoned".to_owned())?
            .get(&ticket)
            .cloned()
    };
    if let Some(shared) = shared {
        let mut unacked = shared
            .unacked
            .lock()
            .map_err(|_| "worker_internal_error: stream lock poisoned".to_owned())?;
        unacked.remove(&seq);
        drop(unacked);
        shared.slot_available.notify_one();
    }
    // Acks delete assets even after the producer finished (terminal cleanup).
    let asset = asset_path(&app, &ticket, seq)?;
    if asset.is_file() {
        let _ = fs::remove_file(asset);
    }
    Ok(())
}

#[tauri::command]
pub fn media_verify_stream_abort(
    app: AppHandle,
    manager: tauri::State<'_, VerifyStreamManager>,
    ticket: String,
) -> Result<(), String> {
    let shared = {
        manager
            .streams
            .lock()
            .map_err(|_| "worker_internal_error: stream registry lock poisoned".to_owned())?
            .get(&ticket)
            .cloned()
    };
    if let Some(shared) = shared {
        shared.cancelled.store(true, Ordering::SeqCst);
        shared.slot_available.notify_all();
    }
    // Best-effort cleanup of any remaining assets for this stream.
    let dir = stream_dir(&app, &ticket)?;
    if dir.is_dir() {
        let _ = fs::remove_dir_all(dir);
    }
    Ok(())
}

fn stream_dir(app: &AppHandle, ticket: &str) -> Result<PathBuf, String> {
    Ok(app
        .path()
        .app_cache_dir()
        .map_err(|error| format!("frame_asset_cache_error: {error}"))?
        .join("frame-assets")
        .join(ticket))
}

fn asset_path(app: &AppHandle, ticket: &str, seq: u64) -> Result<PathBuf, String> {
    Ok(stream_dir(app, ticket)?.join(format!("seq-{seq}.f32le")))
}

/// Apply the scan scope to the frame index: full/preview ranges and selection
/// rules operate on the indexed frame list (decode order).
fn select_frames(
    index_path: &std::path::Path,
    request: &VerifyStreamRequest,
) -> Result<Vec<FrameIdentity>, String> {
    let file = File::open(index_path)
        .map_err(|error| format!("frame_index_cache_error: {error}"))?;
    let reader = std::io::BufReader::new(file);
    let mut frames: Vec<FrameIdentity> = Vec::new();
    for line in std::io::BufRead::lines(reader) {
        let line = line.map_err(|error| format!("frame_index_read_error: {error}"))?;
        if line.trim().is_empty() {
            continue;
        }
        let frame: FrameIdentity = serde_json::from_str(&line)
            .map_err(|error| format!("frame_index_schema_error: {error}"))?;
        frames.push(frame);
    }
    let start = request.start_frame.unwrap_or(0);
    let end = request
        .end_frame
        .unwrap_or_else(|| frames.last().map(|frame| frame.frame_index).unwrap_or(0));
    if start > end {
        return Err("bad_request: range start must be <= end".to_owned());
    }
    let in_range = frames
        .into_iter()
        .filter(|frame| frame.frame_index >= start && frame.frame_index <= end);
    let selected: Vec<FrameIdentity> = match request.selection.as_str() {
        "all" => in_range.collect(),
        "every_n" => {
            let n = request.every_n.unwrap_or(0);
            if n < 1 {
                return Err("bad_request: every-N selection requires everyN >= 1".to_owned());
            }
            in_range
                .filter(|frame| (frame.frame_index - start).is_multiple_of(n))
                .collect()
        }
        "decoded_i_picture" => in_range.filter(|frame| frame.key_frame).collect(),
        other => return Err(format!("bad_request: unknown selection rule {other}")),
    };
    Ok(selected)
}

struct ProducerParams {
    app: AppHandle,
    ticket: String,
    ffmpeg: PathBuf,
    media_path: PathBuf,
    stream_index: u32,
    selected: Vec<FrameIdentity>,
    width: u32,
    height: u32,
    frame_bytes: usize,
    asset_dir: PathBuf,
    in_flight: usize,
    shared: Arc<StreamShared>,
}

fn select_filter(selected: &[FrameIdentity]) -> String {
    // The select expression mirrors the scope: contiguous ranges use between();
    // sparse selections (I-pictures, every-N) use an explicit eq() chain. n is
    // the decode-order counter, identical to the frame index numbering.
    let is_contiguous = selected
        .windows(2)
        .all(|pair| pair[1].frame_index == pair[0].frame_index + 1);
    if is_contiguous {
        let first = selected[0].frame_index;
        let last = selected[selected.len() - 1].frame_index;
        return format!("select='between(n\\,{first}\\,{last})'");
    }
    let mut expression = String::from("select='0");
    for frame in selected {
        expression.push_str(&format!("+eq(n\\,{})", frame.frame_index));
    }
    expression.push('\'');
    expression
}

fn run_producer(params: ProducerParams) -> Result<u64, String> {
    let ProducerParams {
        app,
        ticket,
        ffmpeg,
        media_path,
        stream_index,
        selected,
        width,
        height,
        frame_bytes,
        asset_dir,
        in_flight,
        shared,
    } = params;

    let mut child = Command::new(ffmpeg)
        .args(["-v", "error"])
        .arg("-i")
        .arg(&media_path)
        .args(["-map", &format!("0:{stream_index}")])
        .args(["-vf", &select_filter(&selected)])
        .args(["-f", "rawvideo", "-pix_fmt", "grayf32le", "-"])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|error| format!("video_decode_start_error: {error}"))?;

    let mut stdout = child
        .stdout
        .take()
        .ok_or_else(|| "video_decode_start_error: FFmpeg stdout is unavailable".to_owned())?;

    let mut produced = 0_u64;
    let result = (|| -> Result<(), String> {
        for (seq, frame) in selected.iter().enumerate() {
            if shared.cancelled.load(Ordering::SeqCst) {
                return Err("cancelled".to_owned());
            }
            // Backpressure: wait while too many assets are unacknowledged.
            {
                let mut unacked = shared
                    .unacked
                    .lock()
                    .map_err(|_| "worker_internal_error: stream lock poisoned".to_owned())?;
                while unacked.len() >= in_flight && !shared.cancelled.load(Ordering::SeqCst) {
                    unacked = shared
                        .slot_available
                        .wait(unacked)
                        .map_err(|_| "worker_internal_error: stream lock poisoned".to_owned())?;
                }
                if shared.cancelled.load(Ordering::SeqCst) {
                    return Err("cancelled".to_owned());
                }
            }

            let mut buffer = vec![0_u8; frame_bytes];
            stdout
                .read_exact(&mut buffer)
                .map_err(|error| format!("video_decode_error: {error}"))?;

            let seq = seq as u64;
            let asset_path = asset_dir.join(format!("seq-{seq}.f32le"));
            let temporary = asset_dir.join(format!("seq-{seq}.{}.tmp", Uuid::new_v4()));
            {
                let mut writer = BufWriter::new(
                    File::create(&temporary)
                        .map_err(|error| format!("frame_asset_error: {error}"))?,
                );
                writer
                    .write_all(&buffer)
                    .map_err(|error| format!("frame_asset_error: {error}"))?;
                writer
                    .flush()
                    .map_err(|error| format!("frame_asset_error: {error}"))?;
            }
            fs::rename(&temporary, &asset_path)
                .map_err(|error| format!("frame_asset_error: {error}"))?;

            {
                let mut unacked = shared
                    .unacked
                    .lock()
                    .map_err(|_| "worker_internal_error: stream lock poisoned".to_owned())?;
                unacked.insert(seq);
            }
            app.emit(
                "media-verify-asset",
                serde_json::json!({
                    "ticket": ticket,
                    "seq": seq,
                    "frameIndex": frame.frame_index,
                    "pts": frame.pts,
                    "timestampSeconds": frame.timestamp_seconds,
                    "path": asset_path.display().to_string(),
                    "width": width,
                    "height": height,
                }),
            )
            .map_err(|error| format!("worker_internal_error: {error}"))?;
            produced += 1;
        }
        Ok(())
    })();

    // Closing stdout (read error or completion) makes FFmpeg exit on its own;
    // kill defensively for the cancel path.
    let _ = child.kill();
    let _ = child.wait();
    result.map(|_| produced)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame(index: u64, key_frame: bool) -> FrameIdentity {
        FrameIdentity {
            frame_index: index,
            pts: None,
            best_effort_timestamp: None,
            timestamp_seconds: None,
            key_frame,
            picture_type: None,
        }
    }

    #[test]
    fn selection_rules_partition_the_index() {
        let dir = env_temp();
        let index = dir.join("index.jsonl");
        let mut writer = BufWriter::new(File::create(&index).unwrap());
        for n in 0..10_u64 {
            serde_json::to_writer(&mut writer, &frame(n, n % 4 == 0)).unwrap();
            writer.write_all(b"\n").unwrap();
        }
        writer.flush().unwrap();

        let base = VerifyStreamRequest {
            path: String::new(),
            fingerprint: None,
            stream_index: 0,
            selection: "all".to_owned(),
            every_n: None,
            start_frame: None,
            end_frame: None,
            width: 64,
            height: 64,
            in_flight: None,
        };
        let all = select_frames(&index, &base).unwrap();
        assert_eq!(all.len(), 10);

        let ranged = select_frames(
            &index,
            &VerifyStreamRequest {
                start_frame: Some(2),
                end_frame: Some(5),
                ..base.clone()
            },
        );
        assert_eq!(
            ranged.unwrap().iter().map(|f| f.frame_index).collect::<Vec<_>>(),
            vec![2, 3, 4, 5]
        );

        let every_n = select_frames(
            &index,
            &VerifyStreamRequest {
                selection: "every_n".to_owned(),
                every_n: Some(3),
                start_frame: Some(1),
                end_frame: Some(9),
                ..base.clone()
            },
        )
        .unwrap();
        assert_eq!(
            every_n.iter().map(|f| f.frame_index).collect::<Vec<_>>(),
            vec![1, 4, 7]
        );

        let iframes = select_frames(
            &index,
            &VerifyStreamRequest {
                selection: "decoded_i_picture".to_owned(),
                ..base.clone()
            },
        )
        .unwrap();
        assert_eq!(
            iframes.iter().map(|f| f.frame_index).collect::<Vec<_>>(),
            vec![0, 4, 8]
        );

        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn select_filter_uses_between_for_contiguous_and_eq_chain_for_sparse() {
        let contiguous: Vec<FrameIdentity> = (3..=6).map(|n| frame(n, false)).collect();
        assert_eq!(select_filter(&contiguous), "select='between(n\\,3\\,6)'");

        let sparse = vec![frame(0, true), frame(4, true), frame(8, true)];
        assert_eq!(
            select_filter(&sparse),
            "select='0+eq(n\\,0)+eq(n\\,4)+eq(n\\,8)'"
        );
    }

    fn env_temp() -> PathBuf {
        let dir = std::env::temp_dir().join(format!("getnative_vstream_{}", Uuid::new_v4()));
        fs::create_dir_all(&dir).unwrap();
        dir
    }
}
