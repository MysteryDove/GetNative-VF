use crate::project::manifest::{SourceKind, SourceState, VideoStreamRecord};
use image::codecs::gif::GifDecoder;
use image::codecs::png::PngDecoder;
use image::codecs::webp::WebPDecoder;
use image::{AnimationDecoder, ImageFormat, ImageReader};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs::{self, File};
use std::io::{BufReader, BufWriter, Cursor, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use tauri::{ipc::Response, AppHandle, Manager};
use uuid::Uuid;

const PREVIEW_MAX_DIMENSION: u32 = 1600;
const PREVIEW_MIN_DIMENSION: u32 = 64;
const FINGERPRINT_EDGE_BYTES: usize = 64 * 1024;
const FRAME_ASSET_CACHE_MAX_FILES: usize = 64;
const FRAME_ASSET_CACHE_MAX_BYTES: u64 = 512 * 1024 * 1024;

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
    pub max_dimension: Option<u32>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MediaFrameAssetRequest {
    pub path: String,
    pub fingerprint: Option<String>,
    pub stream_index: Option<u32>,
    pub frame_index: Option<u64>,
    pub width: Option<u32>,
    pub height: Option<u32>,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct MediaFrameAsset {
    pub path: String,
    pub format: String,
    pub width: u32,
    pub height: u32,
    pub from_cache: bool,
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

#[tauri::command]
pub fn media_pick_files() -> Result<Vec<String>, String> {
    Ok(rfd::FileDialog::new()
        .add_filter(
            "Media",
            &[
                "png", "jpg", "jpeg", "gif", "webp", "tif", "tiff", "bmp", "mkv", "mp4",
                "m4v", "mov", "avi", "webm", "ts", "m2ts",
            ],
        )
        .pick_files()
        .unwrap_or_default()
        .into_iter()
        .map(|path| path.display().to_string())
        .collect())
}

#[tauri::command]
pub fn media_probe(request: MediaProbeRequest) -> Result<MediaProbeResult, String> {
    let path = validated_media_path(&request.path)?;
    probe_path(&path)
}

#[tauri::command]
pub async fn media_preview(request: MediaPreviewRequest) -> Result<Response, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let path = validated_media_path(&request.path)?;
        validate_expected_fingerprint(&path, request.fingerprint.as_deref())?;
        if image_format(&path).is_none() {
            return Err(
                "video_engine_required: video previews are decoded by the resident engine".to_owned(),
            );
        }
        let maximum = request.max_dimension.unwrap_or(PREVIEW_MAX_DIMENSION);
        if !(PREVIEW_MIN_DIMENSION..=PREVIEW_MAX_DIMENSION).contains(&maximum) {
            return Err(format!(
                "preview_dimension_invalid: maxDimension must be between {PREVIEW_MIN_DIMENSION} and {PREVIEW_MAX_DIMENSION}"
            ));
        }
        let bytes = render_preview(&path, maximum)?;
        Ok(Response::new(bytes))
    })
    .await
    .map_err(|error| format!("preview_task_error: {error}"))?
}

/// Thumbnail-oriented decode. The pinned `image` 0.25 exposes no
/// decoder-level scaling, so the cheapest path is: decode via `ImageReader`,
/// skip resampling entirely when the source already fits the requested box,
/// otherwise downscale with the fast box-sampling `thumbnail` (imageops)
/// instead of a full-quality resize.
fn render_preview(path: &Path, maximum: u32) -> Result<Vec<u8>, String> {
    let reader = ImageReader::open(path)
        .map_err(|error| format!("image_decode_error: {error}"))?
        .with_guessed_format()
        .map_err(|error| format!("image_decode_error: {error}"))?;
    let image = reader
        .decode()
        .map_err(|error| format!("image_decode_error: {error}"))?;
    let preview = if image.width() <= maximum && image.height() <= maximum {
        image
    } else {
        image.thumbnail(maximum, maximum)
    };
    let mut bytes = Vec::new();
    preview
        .write_to(&mut Cursor::new(&mut bytes), ImageFormat::Png)
        .map_err(|error| format!("image_preview_error: {error}"))?;
    Ok(bytes)
}

#[tauri::command]
pub async fn media_frame_asset(
    app: AppHandle,
    request: MediaFrameAssetRequest,
) -> Result<MediaFrameAsset, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let path = validated_media_path(&request.path)?;
        validate_expected_fingerprint(&path, request.fingerprint.as_deref())?;
        if image_format(&path).is_none() {
            return Err(
                "video_engine_required: video frame assets are decoded by the resident engine"
                    .to_owned(),
            );
        }
        let cache = app
            .path()
            .app_cache_dir()
            .map_err(|error| format!("frame_asset_cache_error: {error}"))?
            .join("frame-assets");
        frame_asset_still(&path, &request, &cache)
    })
    .await
    .map_err(|error| format!("frame_asset_task_error: {error}"))?
}

fn probe_path(path: &Path) -> Result<MediaProbeResult, String> {
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
            kind: if animated { SourceKind::Animated } else { SourceKind::Still },
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
    if looks_like_video(path) {
        return Ok(MediaProbeResult {
            path: path.display().to_string(),
            file_name,
            kind: SourceKind::Video,
            state: SourceState::Probing,
            fingerprint,
            size_bytes: metadata.len(),
            width: None,
            height: None,
            duration_seconds: None,
            decoder: None,
            video_streams: Vec::new(),
            selected_stream_index: None,
            diagnostic: None,
        });
    }
    Ok(MediaProbeResult {
        path: path.display().to_string(),
        file_name,
        kind: SourceKind::Video,
        state: SourceState::Unsupported,
        fingerprint,
        size_bytes: metadata.len(),
        width: None,
        height: None,
        duration_seconds: None,
        decoder: None,
        video_streams: Vec::new(),
        selected_stream_index: None,
        diagnostic: Some(MediaDiagnostic {
            code: "unsupported_media".to_owned(),
            detail: "the selected file is not a supported image or video container".to_owned(),
        }),
    })
}

pub(crate) fn validated_media_path(raw: &str) -> Result<PathBuf, String> {
    let path = PathBuf::from(raw);
    if raw.trim().is_empty() || !path.is_file() {
        return Err(format!("media_missing: media file does not exist: {raw}"));
    }
    path.canonicalize()
        .map_err(|error| format!("media_path_error: failed to resolve media path: {error}"))
}

fn validate_expected_fingerprint(path: &Path, expected: Option<&str>) -> Result<(), String> {
    let Some(expected) = expected else { return Ok(()) };
    let size = fs::metadata(path)
        .map_err(|error| format!("media_read_error: failed to read media metadata: {error}"))?
        .len();
    if quick_fingerprint(path, size)? != expected {
        return Err(
            "media_fingerprint_mismatch: the source file changed after it was imported".to_owned(),
        );
    }
    Ok(())
}

fn quick_fingerprint(path: &Path, size: u64) -> Result<String, String> {
    let mut file = File::open(path)
        .map_err(|error| format!("media_fingerprint_error: failed to open media: {error}"))?;
    let mut hasher = Sha256::new();
    hasher.update(size.to_le_bytes());
    let mut buffer = vec![0_u8; FINGERPRINT_EDGE_BYTES];
    let prefix = file
        .read(&mut buffer)
        .map_err(|error| format!("media_fingerprint_error: failed to read media: {error}"))?;
    hasher.update(&buffer[..prefix]);
    if size > FINGERPRINT_EDGE_BYTES as u64 {
        file.seek(SeekFrom::End(-(FINGERPRINT_EDGE_BYTES as i64)))
            .map_err(|error| format!("media_fingerprint_error: failed to seek media: {error}"))?;
        let suffix = file
            .read(&mut buffer)
            .map_err(|error| format!("media_fingerprint_error: failed to read media: {error}"))?;
        hasher.update(&buffer[..suffix]);
    }
    Ok(format!("quick-sha256-v1:{:x}", hasher.finalize()))
}

fn image_format(path: &Path) -> Option<ImageFormat> {
    ImageReader::open(path)
        .ok()?
        .with_guessed_format()
        .ok()?
        .format()
        .filter(|format| matches!(
            format,
            ImageFormat::Png | ImageFormat::Jpeg | ImageFormat::Gif
                | ImageFormat::WebP | ImageFormat::Tiff | ImageFormat::Bmp
        ))
}

fn is_animated_image(path: &Path, format: ImageFormat) -> Result<bool, String> {
    match format {
        ImageFormat::Gif => {
            let decoder = GifDecoder::new(BufReader::new(
                File::open(path).map_err(|error| format!("image_open_error: {error}"))?,
            ))
            .map_err(|error| format!("image_decode_error: {error}"))?;
            Ok(decoder.into_frames().take(2).count() > 1)
        }
        ImageFormat::Png => {
            let decoder = PngDecoder::new(BufReader::new(
                File::open(path).map_err(|error| format!("image_open_error: {error}"))?,
            ))
            .map_err(|error| format!("image_decode_error: {error}"))?;
            decoder
                .is_apng()
                .map_err(|error| format!("image_decode_error: {error}"))
        }
        ImageFormat::WebP => {
            let decoder = WebPDecoder::new(BufReader::new(
                File::open(path).map_err(|error| format!("image_open_error: {error}"))?,
            ))
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

fn frame_asset_cache_path(
    cache: &Path,
    identity: &str,
    width: u32,
    height: u32,
) -> PathBuf {
    let mut hasher = Sha256::new();
    hasher.update(identity.as_bytes());
    hasher.update(width.to_le_bytes());
    hasher.update(height.to_le_bytes());
    cache.join(format!("{:x}.f32le", hasher.finalize()))
}

pub(crate) fn frame_asset_still(
    path: &Path,
    request: &MediaFrameAssetRequest,
    cache: &Path,
) -> Result<MediaFrameAsset, String> {
    if request.stream_index.is_some() || request.frame_index.is_some() {
        return Err("frame_asset_invalid: still images do not accept stream or frame indices".to_owned());
    }
    let rgb = image::open(path)
        .map_err(|error| format!("image_decode_error: {error}"))?
        .to_rgb32f();
    let (width, height) = (rgb.width(), rgb.height());
    if let (Some(expected_width), Some(expected_height)) = (request.width, request.height) {
        if (expected_width, expected_height) != (width, height) {
            return Err(format!(
                "frame_asset_invalid: expected {expected_width}x{expected_height}, decoded {width}x{height}"
            ));
        }
    }
    let identity = request.fingerprint.as_deref().unwrap_or_else(|| path.to_str().unwrap_or(""));
    let output = frame_asset_cache_path(cache, identity, width, height);
    let expected_bytes = u64::from(width) * u64::from(height) * 4_u64;
    if fs::metadata(&output).is_ok_and(|metadata| metadata.len() == expected_bytes) {
        return Ok(MediaFrameAsset {
            path: output.display().to_string(),
            format: "f32le".to_owned(),
            width,
            height,
            from_cache: true,
        });
    }
    fs::create_dir_all(cache)
        .map_err(|error| format!("frame_asset_cache_error: {error}"))?;
    let temporary = output.with_extension(format!("{}.tmp", Uuid::new_v4()));
    let result = (|| -> Result<(), String> {
        let file = File::create(&temporary)
            .map_err(|error| format!("frame_asset_error: {error}"))?;
        let mut writer = BufWriter::new(file);
        for pixel in rgb.pixels() {
            let luma = 0.2126_f32 * pixel.0[0]
                + 0.7152_f32 * pixel.0[1]
                + 0.0722_f32 * pixel.0[2];
            writer
                .write_all(&luma.to_le_bytes())
                .map_err(|error| format!("frame_asset_error: {error}"))?;
        }
        writer.flush().map_err(|error| format!("frame_asset_error: {error}"))?;
        writer
            .get_ref()
            .sync_all()
            .map_err(|error| format!("frame_asset_error: {error}"))
    })();
    if let Err(error) = result {
        let _ = fs::remove_file(&temporary);
        return Err(error);
    }
    if fs::metadata(&temporary).map(|metadata| metadata.len()).unwrap_or(0) != expected_bytes {
        let _ = fs::remove_file(&temporary);
        return Err("frame_asset_error: decoded still asset is truncated".to_owned());
    }
    match fs::rename(&temporary, &output) {
        Ok(()) => {}
        Err(_) if output.is_file() => {
            let _ = fs::remove_file(&temporary);
        }
        Err(error) => {
            let _ = fs::remove_file(&temporary);
            return Err(format!("frame_asset_cache_error: {error}"));
        }
    }
    prune_frame_asset_cache(cache)?;
    Ok(MediaFrameAsset {
        path: output.display().to_string(),
        format: "f32le".to_owned(),
        width,
        height,
        from_cache: false,
    })
}

fn prune_frame_asset_cache(cache: &Path) -> Result<(), String> {
    let mut files = fs::read_dir(cache)
        .map_err(|error| format!("frame_asset_cache_error: {error}"))?
        .filter_map(Result::ok)
        .filter_map(|entry| {
            let metadata = entry.metadata().ok()?;
            metadata.is_file().then(|| (
                entry.path(), metadata.len(), metadata.modified().ok(),
            ))
        })
        .collect::<Vec<_>>();
    files.sort_by_key(|(_, _, modified)| *modified);
    let mut bytes = files.iter().map(|(_, size, _)| *size).sum::<u64>();
    let mut count = files.len();
    for (path, size, _) in files {
        if count <= FRAME_ASSET_CACHE_MAX_FILES && bytes <= FRAME_ASSET_CACHE_MAX_BYTES {
            break;
        }
        if fs::remove_file(path).is_ok() {
            count = count.saturating_sub(1);
            bytes = bytes.saturating_sub(size);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{ImageBuffer, Rgba};

    #[test]
    fn video_probe_stays_probing_until_engine_index_finishes() {
        let path = std::env::temp_dir().join(format!("getnative-probe-{}.mp4", Uuid::new_v4()));
        fs::write(&path, b"container fixture").unwrap();
        let result = probe_path(&path).unwrap();
        assert_eq!(result.kind, SourceKind::Video);
        assert_eq!(result.state, SourceState::Probing);
        assert!(result.video_streams.is_empty());
        let _ = fs::remove_file(path);
    }

    #[test]
    fn still_asset_is_float_luma_and_cached() {
        let path = std::env::temp_dir().join(format!("getnative-still-{}.png", Uuid::new_v4()));
        ImageBuffer::from_pixel(8, 6, Rgba([12_u8, 34, 56, 255]))
            .save(&path)
            .unwrap();
        let cache = std::env::temp_dir().join(format!("getnative-cache-{}", Uuid::new_v4()));
        let request = MediaFrameAssetRequest {
            path: path.display().to_string(),
            fingerprint: None,
            stream_index: None,
            frame_index: None,
            width: Some(8),
            height: Some(6),
        };
        let first = frame_asset_still(&path, &request, &cache).unwrap();
        assert!(!first.from_cache);
        assert_eq!(fs::metadata(&first.path).unwrap().len(), 8 * 6 * 4);
        assert!(frame_asset_still(&path, &request, &cache).unwrap().from_cache);
        let _ = fs::remove_file(path);
        let _ = fs::remove_dir_all(cache);
    }
}
