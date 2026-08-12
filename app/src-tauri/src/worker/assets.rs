//! Media cache location and cached-asset path safety for worker commands.
//!
//! The engine writes frame assets and PNG previews below the app cache; the
//! helpers here own where that directory lives and which files the frontend
//! is allowed to read back.

use std::fs;
use std::path::PathBuf;
use tauri::{AppHandle, Manager};

const MAX_CACHED_PREVIEW_BYTES: u64 = 64 * 1024 * 1024;

pub(crate) fn media_cache_directory(app: &AppHandle) -> Result<PathBuf, String> {
    let app_cache = app
        .path()
        .app_cache_dir()
        .map_err(|error| format!("media_cache_error: {error}"))?;
    let directory = app_cache.join("media");
    fs::create_dir_all(&directory)
        .map_err(|error| format!("media_cache_error: failed to create cache directory: {error}"))?;
    directory
        .canonicalize()
        .map_err(|error| format!("media_cache_error: failed to resolve cache directory: {error}"))
}

/// One-shot legacy cleanup: remove the retired `media-index` cache. Runs once
/// from the app setup hook instead of on every media command.
pub(crate) fn migrate_legacy_media_cache(app: &AppHandle) -> Result<(), String> {
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
    Ok(())
}

/// Path-safety logic behind `engine_worker_media_read_asset`: resolve the
/// requested path, confine it to PNG files below the media cache, and return
/// verified PNG bytes.
pub(crate) fn read_cached_preview(app: &AppHandle, path: &str) -> Result<Vec<u8>, String> {
    let cache_directory = media_cache_directory(app)?;
    let requested = PathBuf::from(path)
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
    Ok(bytes)
}
