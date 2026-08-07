use crate::project::manifest::{SourceKind, SourceState, VideoStreamRecord};
use image::codecs::gif::GifDecoder;
use image::codecs::png::PngDecoder;
use image::codecs::webp::WebPDecoder;
use image::{AnimationDecoder, ImageFormat, ImageReader};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::env;
use std::fs::{self, File};
use std::io::{BufRead, BufReader, BufWriter, Cursor, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use tauri::{ipc::Response, AppHandle, Manager};
use uuid::Uuid;

const PREVIEW_MAX_DIMENSION: u32 = 1600;
const PREVIEW_MIN_DIMENSION: u32 = 64;
const FINGERPRINT_EDGE_BYTES: usize = 64 * 1024;
const DEFAULT_FRAME_WINDOW_RADIUS: u32 = 12;
const MAX_FRAME_WINDOW_RADIUS: u32 = 120;
const FRAME_PREVIEW_CACHE_MAX_FILES: usize = 512;
const FRAME_PREVIEW_CACHE_MAX_BYTES: u64 = 256 * 1024 * 1024;

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct MediaToolCapability {
    pub available: bool,
    pub source: String,
    pub path: Option<String>,
    pub version: Option<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct MediaCapabilities {
    pub still_formats: Vec<String>,
    pub ffmpeg: MediaToolCapability,
    pub ffprobe: MediaToolCapability,
    pub video_decode_available: bool,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MediaProbeRequest {
    pub path: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MediaPreviewRequest {
    pub path: String,
    pub fingerprint: Option<String>,
    pub stream_index: Option<u32>,
    pub frame_index: Option<u64>,
    pub timestamp_seconds: Option<f64>,
    pub exact: Option<bool>,
    pub max_dimension: Option<u32>,
}

#[derive(Debug, Clone, Copy, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub enum FrameWindowTarget {
    Frame,
    Timestamp,
    PreviousKeyframe,
    NextKeyframe,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MediaFrameWindowRequest {
    pub path: String,
    pub fingerprint: Option<String>,
    pub stream_index: u32,
    pub target: FrameWindowTarget,
    pub frame_index: Option<u64>,
    pub timestamp_seconds: Option<f64>,
    pub window_radius: Option<u32>,
}

#[derive(Debug, Clone, Deserialize, Serialize, PartialEq)]
pub struct FrameIdentity {
    pub frame_index: u64,
    pub pts: Option<i64>,
    pub best_effort_timestamp: Option<i64>,
    pub timestamp_seconds: Option<f64>,
    pub key_frame: bool,
    pub picture_type: Option<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct MediaFrameWindow {
    pub selected: FrameIdentity,
    pub frames: Vec<FrameIdentity>,
    pub total_frames: u64,
    pub previous_keyframe: Option<FrameIdentity>,
    pub next_keyframe: Option<FrameIdentity>,
    pub indexed_complete: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct MediaDiagnostic {
    pub code: String,
    pub detail: String,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct MediaProbeResult {
    pub path: String,
    pub file_name: String,
    pub kind: SourceKind,
    pub state: SourceState,
    pub fingerprint: String,
    pub size_bytes: u64,
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub duration_seconds: Option<f64>,
    pub decoder: Option<String>,
    pub video_streams: Vec<VideoStreamRecord>,
    pub selected_stream_index: Option<u32>,
    pub diagnostic: Option<MediaDiagnostic>,
}

#[derive(Debug, Clone)]
struct MediaTool {
    path: PathBuf,
    source: &'static str,
}

#[tauri::command]
pub fn media_capabilities(app: AppHandle) -> Result<MediaCapabilities, String> {
    let ffmpeg = resolve_media_tool(&app, "ffmpeg", "GETNATIVE_FFMPEG_PATH");
    let ffprobe = resolve_media_tool(&app, "ffprobe", "GETNATIVE_FFPROBE_PATH");
    let video_decode_available = ffmpeg.is_some() && ffprobe.is_some();
    Ok(MediaCapabilities {
        still_formats: ["png", "jpeg", "jpg", "gif", "webp", "tiff", "tif", "bmp"]
            .into_iter()
            .map(str::to_owned)
            .collect(),
        ffmpeg: tool_capability(ffmpeg.as_ref()),
        ffprobe: tool_capability(ffprobe.as_ref()),
        video_decode_available,
    })
}

#[tauri::command]
pub fn media_pick_files() -> Result<Vec<String>, String> {
    Ok(rfd::FileDialog::new()
        .add_filter(
            "Media",
            &[
                "png", "jpg", "jpeg", "gif", "webp", "tif", "tiff", "bmp", "mkv", "mp4", "m4v",
                "mov", "avi", "webm", "ts", "m2ts",
            ],
        )
        .pick_files()
        .unwrap_or_default()
        .into_iter()
        .map(|path| path.display().to_string())
        .collect())
}

#[tauri::command]
pub fn media_probe(app: AppHandle, request: MediaProbeRequest) -> Result<MediaProbeResult, String> {
    let path = validated_media_path(&request.path)?;
    probe_path(
        &path,
        resolve_media_tool(&app, "ffprobe", "GETNATIVE_FFPROBE_PATH").as_ref(),
    )
}

#[tauri::command]
pub fn media_preview(app: AppHandle, request: MediaPreviewRequest) -> Result<Response, String> {
    let path = validated_media_path(&request.path)?;
    validate_expected_fingerprint(&path, request.fingerprint.as_deref())?;
    let max_dimension = preview_max_dimension(&request)?;
    if image_format(&path).is_some() {
        return preview_still(&path, max_dimension).map(Response::new);
    }
    let ffmpeg = resolve_media_tool(&app, "ffmpeg", "GETNATIVE_FFMPEG_PATH")
        .ok_or_else(|| "video_backend_unavailable: bundled FFmpeg is not available".to_owned())?;
    let cache_dir = app
        .path()
        .app_cache_dir()
        .map_err(|error| format!("frame_preview_cache_error: {error}"))?
        .join("media-previews");
    preview_video_cached(&cache_dir, &ffmpeg.path, &path, &request, max_dimension)
        .map(Response::new)
}

#[tauri::command]
pub async fn media_frame_window(
    app: AppHandle,
    request: MediaFrameWindowRequest,
) -> Result<MediaFrameWindow, String> {
    tauri::async_runtime::spawn_blocking(move || frame_window(&app, request))
        .await
        .map_err(|error| format!("frame_index_task_error: {error}"))?
}

fn frame_window(
    app: &AppHandle,
    request: MediaFrameWindowRequest,
) -> Result<MediaFrameWindow, String> {
    let path = validated_media_path(&request.path)?;
    if image_format(&path).is_some() {
        return Err(
            "frame_index_not_applicable: still images do not have a frame index".to_owned(),
        );
    }
    let metadata = fs::metadata(&path)
        .map_err(|error| format!("media_read_error: failed to read media metadata: {error}"))?;
    let fingerprint = quick_fingerprint(&path, metadata.len())?;
    if request
        .fingerprint
        .as_deref()
        .is_some_and(|expected| expected != fingerprint)
    {
        return Err(
            "media_fingerprint_mismatch: the source file changed after it was imported".to_owned(),
        );
    }
    let ffprobe = resolve_media_tool(app, "ffprobe", "GETNATIVE_FFPROBE_PATH")
        .ok_or_else(|| "video_backend_unavailable: bundled FFprobe is not available".to_owned())?;
    let cache_path = frame_index_cache_path(app, &fingerprint, request.stream_index)?;
    if !cache_path.is_file() {
        build_frame_index(&ffprobe.path, &path, request.stream_index, &cache_path)?;
    }
    read_frame_window(&cache_path, &request)
}

fn validated_media_path(raw: &str) -> Result<PathBuf, String> {
    let path = PathBuf::from(raw);
    if raw.trim().is_empty() || !path.is_file() {
        return Err(format!("media_missing: media file does not exist: {raw}"));
    }
    path.canonicalize()
        .map_err(|error| format!("media_path_error: failed to resolve media path: {error}"))
}

fn validate_expected_fingerprint(path: &Path, expected: Option<&str>) -> Result<(), String> {
    let Some(expected) = expected else {
        return Ok(());
    };
    let metadata = fs::metadata(path)
        .map_err(|error| format!("media_read_error: failed to read media metadata: {error}"))?;
    let actual = quick_fingerprint(path, metadata.len())?;
    if actual != expected {
        return Err(
            "media_fingerprint_mismatch: the source file changed after it was imported".to_owned(),
        );
    }
    Ok(())
}

fn probe_path(path: &Path, ffprobe: Option<&MediaTool>) -> Result<MediaProbeResult, String> {
    let metadata = fs::metadata(path)
        .map_err(|error| format!("media_read_error: failed to read media metadata: {error}"))?;
    let fingerprint = quick_fingerprint(path, metadata.len())?;
    let file_name = path
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or_default()
        .to_owned();

    if let Some(format) = image_format(path) {
        let reader = ImageReader::open(path)
            .map_err(|error| format!("image_open_error: {error}"))?
            .with_guessed_format()
            .map_err(|error| format!("image_format_error: {error}"))?;
        let (width, height) = reader
            .into_dimensions()
            .map_err(|error| format!("image_decode_error: {error}"))?;
        let animated = is_animated_image(path, format)?;
        return Ok(MediaProbeResult {
            path: path.display().to_string(),
            file_name,
            kind: if animated {
                SourceKind::Animated
            } else {
                SourceKind::Still
            },
            state: SourceState::Ready,
            fingerprint,
            size_bytes: metadata.len(),
            width: Some(width),
            height: Some(height),
            duration_seconds: None,
            decoder: Some(format!("image-rs/{format:?}")),
            video_streams: Vec::new(),
            selected_stream_index: None,
            diagnostic: None,
        });
    }

    if !looks_like_video(path) {
        return Ok(unsupported_probe(
            path,
            file_name,
            fingerprint,
            metadata.len(),
            "unsupported_media",
            "the selected file is not a supported image or video container",
        ));
    }

    let Some(ffprobe) = ffprobe else {
        return Ok(unsupported_probe(
            path,
            file_name,
            fingerprint,
            metadata.len(),
            "video_backend_unavailable",
            "a bundled FFprobe executable is required for video probing",
        ));
    };
    probe_video(path, file_name, fingerprint, metadata.len(), ffprobe)
}

fn unsupported_probe(
    path: &Path,
    file_name: String,
    fingerprint: String,
    size_bytes: u64,
    code: &str,
    detail: &str,
) -> MediaProbeResult {
    MediaProbeResult {
        path: path.display().to_string(),
        file_name,
        kind: SourceKind::Video,
        state: SourceState::Unsupported,
        fingerprint,
        size_bytes,
        width: None,
        height: None,
        duration_seconds: None,
        decoder: None,
        video_streams: Vec::new(),
        selected_stream_index: None,
        diagnostic: Some(MediaDiagnostic {
            code: code.to_owned(),
            detail: detail.to_owned(),
        }),
    }
}

fn probe_video(
    path: &Path,
    file_name: String,
    fingerprint: String,
    size_bytes: u64,
    ffprobe: &MediaTool,
) -> Result<MediaProbeResult, String> {
    let output = Command::new(&ffprobe.path)
        .args([
            "-v",
            "error",
            "-print_format",
            "json",
            "-show_format",
            "-show_streams",
        ])
        .arg(path)
        .output()
        .map_err(|error| format!("video_probe_start_error: {error}"))?;
    if !output.status.success() {
        return Err(format!(
            "video_probe_error: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    let value: Value = serde_json::from_slice(&output.stdout)
        .map_err(|error| format!("video_probe_schema_error: {error}"))?;
    let duration_seconds = value
        .get("format")
        .and_then(|format| format.get("duration"))
        .and_then(parse_json_f64);
    let video_streams = value
        .get("streams")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter(|stream| stream.get("codec_type").and_then(Value::as_str) == Some("video"))
        .filter_map(parse_video_stream)
        .collect::<Vec<_>>();
    let Some(selected) = video_streams.first() else {
        return Ok(unsupported_probe(
            path,
            file_name,
            fingerprint,
            size_bytes,
            "video_stream_missing",
            "the container does not report a decodable video stream",
        ));
    };
    let selected_index = selected.index;
    let selected_width = selected.width;
    let selected_height = selected.height;
    let selected_duration = selected.duration_seconds;
    Ok(MediaProbeResult {
        path: path.display().to_string(),
        file_name,
        kind: SourceKind::Video,
        state: SourceState::Ready,
        fingerprint,
        size_bytes,
        width: selected_width,
        height: selected_height,
        duration_seconds: selected_duration.or(duration_seconds),
        decoder: Some(format!("FFmpeg ({})", ffprobe.source)),
        video_streams,
        selected_stream_index: Some(selected_index),
        diagnostic: None,
    })
}

fn parse_video_stream(stream: &Value) -> Option<VideoStreamRecord> {
    let index = stream
        .get("index")?
        .as_u64()
        .and_then(|value| u32::try_from(value).ok())?;
    let (time_base_num, time_base_den) = stream
        .get("time_base")
        .and_then(Value::as_str)
        .and_then(parse_rational)
        .map_or((None, None), |(num, den)| (Some(num), Some(den)));
    let (frame_rate_num, frame_rate_den) = stream
        .get("avg_frame_rate")
        .and_then(Value::as_str)
        .and_then(parse_rational)
        .map_or((None, None), |(num, den)| (Some(num), Some(den)));
    Some(VideoStreamRecord {
        index,
        codec_name: stream
            .get("codec_name")
            .and_then(Value::as_str)
            .map(str::to_owned),
        width: stream
            .get("width")
            .and_then(Value::as_u64)
            .and_then(|v| u32::try_from(v).ok()),
        height: stream
            .get("height")
            .and_then(Value::as_u64)
            .and_then(|v| u32::try_from(v).ok()),
        duration_seconds: stream.get("duration").and_then(parse_json_f64),
        frame_count: stream.get("nb_frames").and_then(parse_json_u64),
        time_base_num,
        time_base_den,
        frame_rate_num,
        frame_rate_den,
    })
}

fn parse_json_f64(value: &Value) -> Option<f64> {
    value.as_f64().or_else(|| value.as_str()?.parse().ok())
}

fn parse_json_u64(value: &Value) -> Option<u64> {
    value.as_u64().or_else(|| value.as_str()?.parse().ok())
}

fn parse_rational(value: &str) -> Option<(i64, i64)> {
    let (num, den) = value.split_once('/')?;
    let num = num.parse().ok()?;
    let den = den.parse().ok()?;
    (den != 0).then_some((num, den))
}

fn preview_max_dimension(request: &MediaPreviewRequest) -> Result<u32, String> {
    let max_dimension = request.max_dimension.unwrap_or(PREVIEW_MAX_DIMENSION);
    if !(PREVIEW_MIN_DIMENSION..=PREVIEW_MAX_DIMENSION).contains(&max_dimension) {
        return Err(format!(
            "preview_dimension_invalid: maxDimension must be between {PREVIEW_MIN_DIMENSION} and {PREVIEW_MAX_DIMENSION}"
        ));
    }
    Ok(max_dimension)
}

fn preview_still(path: &Path, max_dimension: u32) -> Result<Vec<u8>, String> {
    let image = image::open(path).map_err(|error| format!("image_decode_error: {error}"))?;
    let preview = image.thumbnail(max_dimension, max_dimension);
    let mut bytes = Vec::new();
    preview
        .write_to(&mut Cursor::new(&mut bytes), ImageFormat::Png)
        .map_err(|error| format!("image_preview_error: {error}"))?;
    Ok(bytes)
}

fn preview_video(
    ffmpeg: &Path,
    path: &Path,
    request: &MediaPreviewRequest,
    max_dimension: u32,
) -> Result<Vec<u8>, String> {
    let stream_index = request.stream_index.unwrap_or(0);
    let exact = request.exact.unwrap_or(true);
    let mut command = Command::new(ffmpeg);
    command.args(["-v", "error"]);
    if !exact {
        let timestamp = request.timestamp_seconds.unwrap_or(0.0).max(0.0);
        command.args(["-ss", &format!("{timestamp:.9}")]);
    }
    command.arg("-i").arg(path);
    command.args(["-map", &format!("0:{stream_index}")]);
    let scale =
        format!("scale=w={max_dimension}:h={max_dimension}:force_original_aspect_ratio=decrease");
    let filter = if exact {
        let frame = request.frame_index.ok_or_else(|| {
            "frame_index_required: exact frame preview requires a frame index".to_owned()
        })?;
        format!("select=eq(n\\,{frame}),{scale}")
    } else {
        scale
    };
    let output = command
        .args([
            "-vf",
            &filter,
            "-frames:v",
            "1",
            "-f",
            "image2pipe",
            "-vcodec",
            "png",
            "pipe:1",
        ])
        .output()
        .map_err(|error| format!("video_decode_start_error: {error}"))?;
    if !output.status.success() || output.stdout.is_empty() {
        return Err(format!(
            "video_decode_error: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    Ok(output.stdout)
}

fn preview_video_cached(
    cache_dir: &Path,
    ffmpeg: &Path,
    path: &Path,
    request: &MediaPreviewRequest,
    max_dimension: u32,
) -> Result<Vec<u8>, String> {
    let cache_path = exact_preview_cache_path(cache_dir, request, max_dimension);
    if let Some(cache_path) = cache_path.as_ref() {
        if let Ok(bytes) = fs::read(cache_path) {
            if bytes.starts_with(b"\x89PNG\r\n\x1a\n") {
                return Ok(bytes);
            }
            let _ = fs::remove_file(cache_path);
        }
    }

    let bytes = preview_video(ffmpeg, path, request, max_dimension)?;
    if let Some(cache_path) = cache_path {
        if fs::create_dir_all(cache_dir).is_ok() {
            let temporary = cache_path.with_extension(format!("{}.tmp", Uuid::new_v4()));
            if fs::write(&temporary, &bytes).is_ok() {
                match fs::rename(&temporary, &cache_path) {
                    Ok(()) => {}
                    Err(_) if cache_path.is_file() => {
                        let _ = fs::remove_file(&temporary);
                    }
                    Err(_) => {
                        let _ = fs::remove_file(&temporary);
                    }
                }
            }
            let _ = prune_frame_preview_cache(cache_dir);
        }
    }
    Ok(bytes)
}

fn exact_preview_cache_path(
    cache_dir: &Path,
    request: &MediaPreviewRequest,
    max_dimension: u32,
) -> Option<PathBuf> {
    if !request.exact.unwrap_or(true) {
        return None;
    }
    let fingerprint = request.fingerprint.as_deref()?;
    let frame_index = request.frame_index?;
    let stream_index = request.stream_index.unwrap_or(0);
    let mut hasher = Sha256::new();
    hasher.update(fingerprint.as_bytes());
    hasher.update(stream_index.to_le_bytes());
    hasher.update(frame_index.to_le_bytes());
    hasher.update(max_dimension.to_le_bytes());
    Some(cache_dir.join(format!("{:x}.png", hasher.finalize())))
}

fn prune_frame_preview_cache(cache_dir: &Path) -> Result<(), String> {
    let mut files = fs::read_dir(cache_dir)
        .map_err(|error| format!("frame_preview_cache_error: {error}"))?
        .filter_map(Result::ok)
        .filter_map(|entry| {
            let metadata = entry.metadata().ok()?;
            metadata
                .is_file()
                .then(|| (entry.path(), metadata.len(), metadata.modified().ok()))
        })
        .collect::<Vec<_>>();
    files.sort_by_key(|(_, _, modified)| *modified);
    let mut total_bytes = files.iter().map(|(_, size, _)| *size).sum::<u64>();
    let mut total_files = files.len();
    for (path, size, _) in files {
        if total_files <= FRAME_PREVIEW_CACHE_MAX_FILES
            && total_bytes <= FRAME_PREVIEW_CACHE_MAX_BYTES
        {
            break;
        }
        if fs::remove_file(path).is_ok() {
            total_files = total_files.saturating_sub(1);
            total_bytes = total_bytes.saturating_sub(size);
        }
    }
    Ok(())
}

fn frame_index_cache_path(
    app: &AppHandle,
    fingerprint: &str,
    stream_index: u32,
) -> Result<PathBuf, String> {
    let mut hasher = Sha256::new();
    hasher.update(fingerprint.as_bytes());
    let key = format!("{:x}", hasher.finalize());
    app.path()
        .app_cache_dir()
        .map(|path| {
            path.join("media-index")
                .join(format!("{key}-{stream_index}.jsonl"))
        })
        .map_err(|error| format!("frame_index_cache_error: {error}"))
}

fn build_frame_index(
    ffprobe: &Path,
    media_path: &Path,
    stream_index: u32,
    cache_path: &Path,
) -> Result<(), String> {
    if let Some(parent) = cache_path.parent() {
        fs::create_dir_all(parent).map_err(|error| format!("frame_index_cache_error: {error}"))?;
    }
    let temporary_path = cache_path.with_extension(format!("{}.tmp", Uuid::new_v4()));
    let result = (|| -> Result<(), String> {
        let mut child = Command::new(ffprobe)
            .args([
                "-v",
                "error",
                "-select_streams",
                &stream_index.to_string(),
                "-show_frames",
                "-show_entries",
                "frame=stream_index,key_frame,pict_type,pts,best_effort_timestamp,best_effort_timestamp_time",
                "-of",
                "compact=p=0:nk=0",
            ])
            .arg(media_path)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .map_err(|error| format!("frame_index_start_error: {error}"))?;

        let stdout = child
            .stdout
            .take()
            .ok_or_else(|| "frame_index_start_error: FFprobe stdout is unavailable".to_owned())?;
        let mut writer = BufWriter::new(
            File::create(&temporary_path)
                .map_err(|error| format!("frame_index_cache_error: {error}"))?,
        );
        let mut frame_index = 0_u64;
        for line in BufReader::new(stdout).lines() {
            let line = line.map_err(|error| format!("frame_index_read_error: {error}"))?;
            let Some(frame) = parse_compact_frame_line(&line, stream_index, frame_index)? else {
                continue;
            };
            serde_json::to_writer(&mut writer, &frame)
                .map_err(|error| format!("frame_index_cache_error: {error}"))?;
            writer
                .write_all(b"\n")
                .map_err(|error| format!("frame_index_cache_error: {error}"))?;
            frame_index += 1;
        }
        writer
            .flush()
            .map_err(|error| format!("frame_index_cache_error: {error}"))?;
        writer
            .get_ref()
            .sync_all()
            .map_err(|error| format!("frame_index_cache_error: {error}"))?;
        drop(writer);

        let mut stderr = String::new();
        if let Some(mut stream) = child.stderr.take() {
            stream
                .read_to_string(&mut stderr)
                .map_err(|error| format!("frame_index_read_error: {error}"))?;
        }
        let status = child
            .wait()
            .map_err(|error| format!("frame_index_wait_error: {error}"))?;
        if !status.success() {
            return Err(format!("frame_index_error: {}", stderr.trim()));
        }
        if frame_index == 0 {
            return Err("frame_index_empty: FFprobe returned no video frames".to_owned());
        }

        match fs::rename(&temporary_path, cache_path) {
            Ok(()) => Ok(()),
            Err(_) if cache_path.is_file() => Ok(()),
            Err(error) => Err(format!("frame_index_cache_error: {error}")),
        }
    })();
    if result.is_err() || cache_path.is_file() {
        let _ = fs::remove_file(&temporary_path);
    }
    result
}

fn parse_compact_frame_line(
    line: &str,
    expected_stream_index: u32,
    frame_index: u64,
) -> Result<Option<FrameIdentity>, String> {
    let mut stream_index = None;
    let mut pts = None;
    let mut best_effort_timestamp = None;
    let mut timestamp_seconds = None;
    let mut key_frame = false;
    let mut picture_type = None;

    for field in line.split('|') {
        let Some((key, value)) = field.split_once('=') else {
            continue;
        };
        match key {
            "stream_index" => stream_index = value.parse::<u32>().ok(),
            "pts" if value != "N/A" => pts = value.parse::<i64>().ok(),
            "best_effort_timestamp" if value != "N/A" => {
                best_effort_timestamp = value.parse::<i64>().ok()
            }
            "best_effort_timestamp_time" if value != "N/A" => {
                timestamp_seconds = value.parse::<f64>().ok()
            }
            "key_frame" => key_frame = value == "1",
            "pict_type" if !value.is_empty() && value != "N/A" => {
                picture_type = Some(value.to_owned())
            }
            _ => {}
        }
    }

    if stream_index != Some(expected_stream_index) {
        return Ok(None);
    }
    if timestamp_seconds.is_some_and(|value| !value.is_finite()) {
        return Err("frame_index_schema_error: non-finite frame timestamp".to_owned());
    }
    Ok(Some(FrameIdentity {
        frame_index,
        pts,
        best_effort_timestamp,
        timestamp_seconds,
        key_frame,
        picture_type,
    }))
}

fn read_frame_window(
    cache_path: &Path,
    request: &MediaFrameWindowRequest,
) -> Result<MediaFrameWindow, String> {
    let target_frame = request.frame_index.unwrap_or(0);
    let target_timestamp = request.timestamp_seconds.unwrap_or(0.0);
    if matches!(request.target, FrameWindowTarget::Timestamp)
        && (!target_timestamp.is_finite() || target_timestamp < 0.0)
    {
        return Err(
            "frame_target_invalid: timestamp must be a finite non-negative value".to_owned(),
        );
    }

    let mut first = None;
    let mut nearest = None;
    let mut nearest_distance = f64::INFINITY;
    let mut previous_keyframe = None;
    let mut next_keyframe = None;
    let mut total_frames = 0_u64;

    for frame in frame_cache_reader(cache_path)? {
        let frame = frame?;
        first.get_or_insert_with(|| frame.clone());
        total_frames += 1;
        let distance = if matches!(request.target, FrameWindowTarget::Timestamp) {
            frame
                .timestamp_seconds
                .map(|timestamp| (timestamp - target_timestamp).abs())
                .unwrap_or(f64::INFINITY)
        } else {
            frame.frame_index.abs_diff(target_frame) as f64
        };
        if distance < nearest_distance {
            nearest_distance = distance;
            nearest = Some(frame.clone());
        }
        match request.target {
            FrameWindowTarget::Frame | FrameWindowTarget::Timestamp => {}
            FrameWindowTarget::PreviousKeyframe => {
                if frame.key_frame && frame.frame_index < target_frame {
                    previous_keyframe = Some(frame.clone());
                }
            }
            FrameWindowTarget::NextKeyframe => {
                if next_keyframe.is_none() && frame.key_frame && frame.frame_index > target_frame {
                    next_keyframe = Some(frame.clone());
                }
            }
        }
    }

    let selected = match request.target {
        FrameWindowTarget::PreviousKeyframe => previous_keyframe
            .clone()
            .or_else(|| nearest.clone())
            .or(first.clone()),
        FrameWindowTarget::NextKeyframe => next_keyframe
            .clone()
            .or_else(|| nearest.clone())
            .or(first.clone()),
        FrameWindowTarget::Frame | FrameWindowTarget::Timestamp => nearest.or(first.clone()),
    }
    .ok_or_else(|| "frame_index_empty: cached index contains no frames".to_owned())?;

    let radius = request
        .window_radius
        .unwrap_or(DEFAULT_FRAME_WINDOW_RADIUS)
        .min(MAX_FRAME_WINDOW_RADIUS) as u64;
    let first_index = selected.frame_index.saturating_sub(radius);
    let last_index = selected.frame_index.saturating_add(radius);
    let mut frames = Vec::new();
    previous_keyframe = None;
    next_keyframe = None;
    for frame in frame_cache_reader(cache_path)? {
        let frame = frame?;
        if frame.key_frame && frame.frame_index < selected.frame_index {
            previous_keyframe = Some(frame.clone());
        }
        if next_keyframe.is_none() && frame.key_frame && frame.frame_index > selected.frame_index {
            next_keyframe = Some(frame.clone());
        }
        if (first_index..=last_index).contains(&frame.frame_index) {
            frames.push(frame);
        }
    }

    Ok(MediaFrameWindow {
        selected,
        frames,
        total_frames,
        previous_keyframe,
        next_keyframe,
        indexed_complete: true,
    })
}

fn frame_cache_reader(
    cache_path: &Path,
) -> Result<impl Iterator<Item = Result<FrameIdentity, String>>, String> {
    let file =
        File::open(cache_path).map_err(|error| format!("frame_index_cache_error: {error}"))?;
    Ok(BufReader::new(file)
        .lines()
        .enumerate()
        .map(|(line, value)| {
            let value = value.map_err(|error| format!("frame_index_cache_error: {error}"))?;
            serde_json::from_str(&value)
                .map_err(|error| format!("frame_index_cache_error at line {}: {error}", line + 1))
        }))
}

fn quick_fingerprint(path: &Path, size: u64) -> Result<String, String> {
    let mut file = File::open(path)
        .map_err(|error| format!("media_fingerprint_error: failed to open media: {error}"))?;
    let mut hasher = Sha256::new();
    hasher.update(size.to_le_bytes());
    let mut buffer = vec![0_u8; FINGERPRINT_EDGE_BYTES];
    let prefix_len = file
        .read(&mut buffer)
        .map_err(|error| format!("media_fingerprint_error: failed to read media: {error}"))?;
    hasher.update(&buffer[..prefix_len]);
    if size > FINGERPRINT_EDGE_BYTES as u64 {
        file.seek(SeekFrom::End(-(FINGERPRINT_EDGE_BYTES as i64)))
            .map_err(|error| format!("media_fingerprint_error: failed to seek media: {error}"))?;
        let suffix_len = file
            .read(&mut buffer)
            .map_err(|error| format!("media_fingerprint_error: failed to read media: {error}"))?;
        hasher.update(&buffer[..suffix_len]);
    }
    Ok(format!("quick-sha256-v1:{:x}", hasher.finalize()))
}

fn image_format(path: &Path) -> Option<ImageFormat> {
    ImageReader::open(path)
        .ok()?
        .with_guessed_format()
        .ok()?
        .format()
        .filter(|format| {
            matches!(
                format,
                ImageFormat::Png
                    | ImageFormat::Jpeg
                    | ImageFormat::Gif
                    | ImageFormat::WebP
                    | ImageFormat::Tiff
                    | ImageFormat::Bmp
            )
        })
}

fn is_animated_image(path: &Path, format: ImageFormat) -> Result<bool, String> {
    match format {
        ImageFormat::Gif => {
            let decoder =
                GifDecoder::new(BufReader::new(File::open(path).map_err(|e| e.to_string())?))
                    .map_err(|error| format!("image_decode_error: {error}"))?;
            Ok(decoder.into_frames().take(2).count() > 1)
        }
        ImageFormat::Png => {
            let decoder =
                PngDecoder::new(BufReader::new(File::open(path).map_err(|e| e.to_string())?))
                    .map_err(|error| format!("image_decode_error: {error}"))?;
            decoder
                .is_apng()
                .map_err(|error| format!("image_decode_error: {error}"))
        }
        ImageFormat::WebP => {
            let decoder =
                WebPDecoder::new(BufReader::new(File::open(path).map_err(|e| e.to_string())?))
                    .map_err(|error| format!("image_decode_error: {error}"))?;
            Ok(decoder.has_animation())
        }
        _ => Ok(false),
    }
}

fn looks_like_video(path: &Path) -> bool {
    matches!(
        path.extension()
            .and_then(|extension| extension.to_str())
            .map(str::to_ascii_lowercase)
            .as_deref(),
        Some("mkv" | "mp4" | "m4v" | "mov" | "avi" | "webm" | "ts" | "m2ts")
    )
}

fn resolve_media_tool(app: &AppHandle, name: &str, environment: &str) -> Option<MediaTool> {
    if let Some(path) = env::var_os(environment)
        .map(PathBuf::from)
        .filter(|path| path.is_file())
    {
        return Some(MediaTool {
            path,
            source: "environment",
        });
    }
    let executable = format!("{name}{}", env::consts::EXE_SUFFIX);
    if let Ok(resource_dir) = app.path().resource_dir() {
        let path = resource_dir.join("bin").join(&executable);
        if path.is_file() {
            return Some(MediaTool {
                path,
                source: "bundled",
            });
        }
    }
    if cfg!(debug_assertions) {
        if let Ok(current_dir) = env::current_dir() {
            for path in [
                current_dir.join("bundle-stage/bin").join(&executable),
                current_dir
                    .join("src-tauri/bundle-stage/bin")
                    .join(&executable),
            ] {
                if path.is_file() {
                    return Some(MediaTool {
                        path,
                        source: "development",
                    });
                }
            }
        }
        if Command::new(&executable).arg("-version").output().is_ok() {
            return Some(MediaTool {
                path: PathBuf::from(executable),
                source: "system-development",
            });
        }
    }
    None
}

fn tool_capability(tool: Option<&MediaTool>) -> MediaToolCapability {
    let version = tool.and_then(|tool| {
        let output = Command::new(&tool.path).arg("-version").output().ok()?;
        output.status.success().then(|| {
            String::from_utf8_lossy(&output.stdout)
                .lines()
                .next()
                .unwrap_or_default()
                .to_owned()
        })
    });
    MediaToolCapability {
        available: tool.is_some(),
        source: tool.map_or("unavailable", |tool| tool.source).to_owned(),
        path: tool.map(|tool| tool.path.display().to_string()),
        version,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{ImageBuffer, Rgba};
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_png() -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let path = env::temp_dir().join(format!("getnative_media_{nanos}.png"));
        let image = ImageBuffer::from_pixel(8, 6, Rgba([12_u8, 34, 56, 255]));
        image.save(&path).unwrap();
        path
    }

    #[test]
    fn still_probe_and_preview_are_real_and_bounded() {
        let path = temp_png();
        let probe = probe_path(&path, None).unwrap();
        assert_eq!(probe.kind, SourceKind::Still);
        assert_eq!(probe.state, SourceState::Ready);
        assert_eq!(probe.width, Some(8));
        assert_eq!(probe.height, Some(6));
        assert!(probe.fingerprint.starts_with("quick-sha256-v1:"));
        let preview = preview_still(&path, PREVIEW_MAX_DIMENSION).unwrap();
        assert_eq!(&preview[..8], b"\x89PNG\r\n\x1a\n");
        validate_expected_fingerprint(&path, Some(&probe.fingerprint)).unwrap();
        fs::write(&path, b"changed").unwrap();
        assert!(validate_expected_fingerprint(&path, Some(&probe.fingerprint)).is_err());
        let _ = fs::remove_file(path);
    }

    #[test]
    fn video_is_preserved_when_the_decoder_is_unavailable() {
        let path = env::temp_dir().join("getnative_media_no_decoder.mkv");
        fs::write(&path, b"fixture").unwrap();
        let probe = probe_path(&path, None).unwrap();
        assert_eq!(probe.kind, SourceKind::Video);
        assert_eq!(probe.state, SourceState::Unsupported);
        assert_eq!(
            probe
                .diagnostic
                .as_ref()
                .map(|diagnostic| diagnostic.code.as_str()),
            Some("video_backend_unavailable")
        );
        let _ = fs::remove_file(path);
    }

    #[test]
    fn rational_parser_rejects_zero_denominator() {
        assert_eq!(parse_rational("24000/1001"), Some((24000, 1001)));
        assert_eq!(parse_rational("1/0"), None);
    }

    #[test]
    fn preview_dimensions_are_bounded_and_partition_the_cache() {
        let request = |max_dimension| MediaPreviewRequest {
            path: String::new(),
            fingerprint: Some("fingerprint".to_owned()),
            stream_index: Some(2),
            frame_index: Some(17),
            timestamp_seconds: None,
            exact: Some(true),
            max_dimension,
        };
        assert_eq!(
            preview_max_dimension(&request(None)).unwrap(),
            PREVIEW_MAX_DIMENSION
        );
        assert!(preview_max_dimension(&request(Some(PREVIEW_MIN_DIMENSION - 1))).is_err());
        assert!(preview_max_dimension(&request(Some(PREVIEW_MAX_DIMENSION + 1))).is_err());

        let cache_dir = Path::new("/tmp/getnative-preview-cache-key");
        let large = exact_preview_cache_path(cache_dir, &request(None), PREVIEW_MAX_DIMENSION);
        let thumbnail = exact_preview_cache_path(cache_dir, &request(Some(160)), 160);
        assert_ne!(large, thumbnail);
    }

    #[test]
    fn compact_frame_parser_preserves_exact_identity() {
        let frame = parse_compact_frame_line(
            "stream_index=2|key_frame=1|pts=999|best_effort_timestamp=1001|best_effort_timestamp_time=0.041708|pict_type=I",
            2,
            17,
        )
        .unwrap()
        .unwrap();
        assert_eq!(frame.frame_index, 17);
        assert_eq!(frame.pts, Some(999));
        assert_eq!(frame.best_effort_timestamp, Some(1001));
        assert_eq!(frame.timestamp_seconds, Some(0.041708));
        assert!(frame.key_frame);
        assert_eq!(frame.picture_type.as_deref(), Some("I"));
        assert!(
            parse_compact_frame_line("stream_index=3|key_frame=0", 2, 18)
                .unwrap()
                .is_none()
        );
    }

    #[test]
    fn frame_window_supports_frame_time_and_keyframe_targets() {
        let path = env::temp_dir().join(format!("getnative_frame_index_{}.jsonl", Uuid::new_v4()));
        let mut writer = BufWriter::new(File::create(&path).unwrap());
        for index in 0..8_u64 {
            serde_json::to_writer(
                &mut writer,
                &FrameIdentity {
                    frame_index: index,
                    pts: Some(index as i64 * 1001),
                    best_effort_timestamp: Some(index as i64 * 1001),
                    timestamp_seconds: Some(index as f64 / 24.0),
                    key_frame: index == 0 || index == 5,
                    picture_type: Some(if index == 0 || index == 5 { "I" } else { "P" }.to_owned()),
                },
            )
            .unwrap();
            writer.write_all(b"\n").unwrap();
        }
        writer.flush().unwrap();

        let request = |target, frame_index, timestamp_seconds| MediaFrameWindowRequest {
            path: String::new(),
            fingerprint: None,
            stream_index: 0,
            target,
            frame_index,
            timestamp_seconds,
            window_radius: Some(1),
        };
        let by_frame =
            read_frame_window(&path, &request(FrameWindowTarget::Frame, Some(3), None)).unwrap();
        assert_eq!(by_frame.selected.frame_index, 3);
        assert_eq!(by_frame.frames.len(), 3);
        assert_eq!(by_frame.previous_keyframe.unwrap().frame_index, 0);
        assert_eq!(by_frame.next_keyframe.unwrap().frame_index, 5);

        let by_time = read_frame_window(
            &path,
            &request(FrameWindowTarget::Timestamp, None, Some(0.21)),
        )
        .unwrap();
        assert_eq!(by_time.selected.frame_index, 5);

        let previous = read_frame_window(
            &path,
            &request(FrameWindowTarget::PreviousKeyframe, Some(7), None),
        )
        .unwrap();
        assert_eq!(previous.selected.frame_index, 5);
        let next = read_frame_window(
            &path,
            &request(FrameWindowTarget::NextKeyframe, Some(2), None),
        )
        .unwrap();
        assert_eq!(next.selected.frame_index, 5);
        let no_previous = read_frame_window(
            &path,
            &request(FrameWindowTarget::PreviousKeyframe, Some(0), None),
        )
        .unwrap();
        assert_eq!(no_previous.selected.frame_index, 0);
        let no_next = read_frame_window(
            &path,
            &request(FrameWindowTarget::NextKeyframe, Some(7), None),
        )
        .unwrap();
        assert_eq!(no_next.selected.frame_index, 7);
        let _ = fs::remove_file(path);
    }

    #[test]
    #[ignore = "requires GETNATIVE_MEDIA_SMOKE_FIXTURE and staged FFmpeg sidecars"]
    fn staged_sidecars_preserve_vfr_frame_identity_and_preview() {
        let fixture = PathBuf::from(
            env::var_os("GETNATIVE_MEDIA_SMOKE_FIXTURE")
                .expect("GETNATIVE_MEDIA_SMOKE_FIXTURE must point to the VFR fixture"),
        );
        let ffmpeg = PathBuf::from(
            env::var_os("GETNATIVE_FFMPEG_PATH")
                .expect("GETNATIVE_FFMPEG_PATH must point to staged FFmpeg"),
        );
        let ffprobe = PathBuf::from(
            env::var_os("GETNATIVE_FFPROBE_PATH")
                .expect("GETNATIVE_FFPROBE_PATH must point to staged FFprobe"),
        );
        let tool = MediaTool {
            path: ffprobe.clone(),
            source: "smoke-test",
        };

        let probe = probe_path(&fixture, Some(&tool)).unwrap();
        assert_eq!(probe.kind, SourceKind::Video);
        assert_eq!(probe.state, SourceState::Ready);
        assert_eq!(probe.video_streams.len(), 2);
        assert_eq!(probe.selected_stream_index, Some(0));
        assert_eq!(probe.width, Some(160));
        assert_eq!(probe.height, Some(90));
        assert_eq!(probe.video_streams[0].frame_count, None);

        let cache_path =
            env::temp_dir().join(format!("getnative_media_smoke_{}.jsonl", Uuid::new_v4()));
        build_frame_index(&ffprobe, &fixture, 0, &cache_path).unwrap();

        let request = |target, frame_index, timestamp_seconds| MediaFrameWindowRequest {
            path: fixture.display().to_string(),
            fingerprint: Some(probe.fingerprint.clone()),
            stream_index: 0,
            target,
            frame_index,
            timestamp_seconds,
            window_radius: Some(2),
        };
        let by_frame = read_frame_window(
            &cache_path,
            &request(FrameWindowTarget::Frame, Some(17), None),
        )
        .unwrap();
        assert_eq!(by_frame.total_frames, 42);
        assert_eq!(by_frame.selected.frame_index, 17);
        assert_eq!(by_frame.selected.pts, Some(1167));
        assert_eq!(by_frame.selected.best_effort_timestamp, Some(1167));
        assert_eq!(by_frame.previous_keyframe.unwrap().frame_index, 12);
        assert_eq!(by_frame.next_keyframe.unwrap().frame_index, 18);

        let timestamp = by_frame.selected.timestamp_seconds.unwrap();
        let by_time = read_frame_window(
            &cache_path,
            &request(FrameWindowTarget::Timestamp, None, Some(timestamp)),
        )
        .unwrap();
        assert_eq!(by_time.selected, by_frame.selected);

        let preview_request = MediaPreviewRequest {
            path: fixture.display().to_string(),
            fingerprint: Some(probe.fingerprint.clone()),
            stream_index: Some(0),
            frame_index: Some(17),
            timestamp_seconds: None,
            exact: Some(true),
            max_dimension: None,
        };
        let preview_cache =
            env::temp_dir().join(format!("getnative_media_preview_cache_{}", Uuid::new_v4()));
        let first_preview = preview_video_cached(
            &preview_cache,
            &ffmpeg,
            &fixture,
            &preview_request,
            PREVIEW_MAX_DIMENSION,
        )
        .unwrap();
        let second_preview = preview_video_cached(
            &preview_cache,
            Path::new("/definitely/missing/ffmpeg"),
            &fixture,
            &preview_request,
            PREVIEW_MAX_DIMENSION,
        )
        .unwrap();
        assert_eq!(&first_preview[..8], b"\x89PNG\r\n\x1a\n");
        assert_eq!(second_preview, first_preview);
        assert_eq!(fs::read_dir(&preview_cache).unwrap().count(), 1);

        let thumbnail_request = MediaPreviewRequest {
            max_dimension: Some(80),
            ..preview_request.clone()
        };
        let thumbnail =
            preview_video_cached(&preview_cache, &ffmpeg, &fixture, &thumbnail_request, 80)
                .unwrap();
        let decoded = image::load_from_memory(&thumbnail).unwrap();
        assert!(decoded.width() <= 80);
        assert!(decoded.height() <= 80);
        assert_eq!(fs::read_dir(&preview_cache).unwrap().count(), 2);

        let _ = fs::remove_file(cache_path);
        let _ = fs::remove_dir_all(preview_cache);
    }
}
