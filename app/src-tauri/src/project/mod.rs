pub mod manifest;
pub mod store;

use manifest::{ManifestErrorCode, ManifestValidationError, ProjectManifest};
use serde::Deserialize;
use std::path::{Path, PathBuf};
use std::process::Command;
use store::{
    clear_recovery, create_project_at_path, create_untitled_project, open_manifest_at_path,
    read_recent, recovery_info, remove_recent_entry, save_project, touch_recent,
    ProjectCommandResult, RecoveryInfo,
};
use tauri::AppHandle;

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CreateProjectRequest {
    pub name: Option<String>,
    pub path: Option<String>,
    pub pick_path: Option<bool>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct OpenProjectRequest {
    pub path: Option<String>,
    pub pick_path: Option<bool>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SaveProjectRequest {
    pub path: Option<String>,
    pub manifest: ProjectManifest,
    pub pick_path: Option<bool>,
    pub dialog_name: Option<String>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RemoveRecentRequest {
    pub path: String,
}

#[derive(Debug, Deserialize)]
pub struct RevealProjectRequest {
    pub path: String,
}

fn cancelled() -> ManifestValidationError {
    ManifestValidationError {
        code: ManifestErrorCode::Cancelled,
        message: "user cancelled the file dialog".to_owned(),
    }
}

fn pick_save_path(default_name: &str) -> Result<String, ManifestValidationError> {
    let file_name = if default_name.ends_with(manifest::PROJECT_FILE_EXTENSION) {
        default_name.to_owned()
    } else {
        format!("{default_name}.{}", manifest::PROJECT_FILE_EXTENSION)
    };
    rfd::FileDialog::new()
        .set_file_name(&file_name)
        .add_filter("GetNative", &[manifest::PROJECT_FILE_EXTENSION])
        .save_file()
        .map(|path| path.display().to_string())
        .ok_or_else(cancelled)
}

fn pick_open_path() -> Result<String, ManifestValidationError> {
    rfd::FileDialog::new()
        .add_filter("GetNative", &[manifest::PROJECT_FILE_EXTENSION])
        .add_filter("JSON", &["json"])
        .pick_file()
        .map(|path| path.display().to_string())
        .ok_or_else(cancelled)
}

fn validate_reveal_path(path: &Path) -> Result<PathBuf, ManifestValidationError> {
    let path = manifest::normalize_project_path(path)?;
    if !path.is_file() {
        return Err(ManifestValidationError {
            code: ManifestErrorCode::IoError,
            message: format!("project file does not exist: {}", path.display()),
        });
    }
    Ok(path)
}

fn reveal_project_path(path: &Path) -> Result<(), ManifestValidationError> {
    let path = validate_reveal_path(path)?;

    #[cfg(target_os = "macos")]
    let status = Command::new("/usr/bin/open").arg("-R").arg(&path).status();

    #[cfg(target_os = "windows")]
    let status = Command::new("explorer.exe")
        .arg(format!("/select,{}", path.display()))
        .status();

    #[cfg(all(unix, not(target_os = "macos")))]
    let status = Command::new("xdg-open")
        .arg(path.parent().unwrap_or(Path::new("/")))
        .status();

    let status = status.map_err(|error| ManifestValidationError {
        code: ManifestErrorCode::IoError,
        message: format!("failed to show project in its folder: {error}"),
    })?;
    if !status.success() {
        return Err(ManifestValidationError {
            code: ManifestErrorCode::IoError,
            message: format!("show project command exited with status {status}"),
        });
    }
    Ok(())
}

#[tauri::command]
pub fn project_create(
    app: AppHandle,
    request: CreateProjectRequest,
) -> Result<ProjectCommandResult, String> {
    let name = request
        .name
        .as_deref()
        .map(str::trim)
        .filter(|value| !value.is_empty());
    let Some(name) = name else {
        return Ok(ProjectCommandResult::failure(ManifestValidationError {
            code: ManifestErrorCode::MissingName,
            message: "project name is required".to_owned(),
        }));
    };

    let path = if request.path.is_some() {
        request.path
    } else if request.pick_path.unwrap_or(true) {
        match pick_save_path(name) {
            Ok(path) => Some(path),
            Err(error) => return Ok(ProjectCommandResult::failure(error)),
        }
    } else {
        None
    };

    let result = if let Some(path) = path {
        create_project_at_path(std::path::Path::new(&path), name)
    } else {
        create_untitled_project(&app)
    };

    match result {
        Ok(opened) => {
            let warnings = touch_recent(&app, &opened).err().into_iter().collect();
            Ok(ProjectCommandResult::success_opened_with_warnings(
                opened, warnings,
            ))
        }
        Err(error) => Ok(ProjectCommandResult::failure(error)),
    }
}

#[tauri::command]
pub fn project_create_untitled(app: AppHandle) -> Result<ProjectCommandResult, String> {
    match create_untitled_project(&app) {
        Ok(opened) => Ok(ProjectCommandResult::success_opened(opened)),
        Err(error) => Ok(ProjectCommandResult::failure(error)),
    }
}

#[tauri::command]
pub fn project_open(
    app: AppHandle,
    request: OpenProjectRequest,
) -> Result<ProjectCommandResult, String> {
    let path = if let Some(path) = request.path {
        path
    } else if request.pick_path.unwrap_or(true) {
        match pick_open_path() {
            Ok(path) => path,
            Err(error) => return Ok(ProjectCommandResult::failure(error)),
        }
    } else {
        return Ok(ProjectCommandResult::failure(ManifestValidationError {
            code: ManifestErrorCode::PathRejected,
            message: "open path is required when pick_path is false".to_owned(),
        }));
    };

    match open_manifest_at_path(std::path::Path::new(&path)) {
        Ok(opened) => {
            let warnings = touch_recent(&app, &opened).err().into_iter().collect();
            Ok(ProjectCommandResult::success_opened_with_warnings(
                opened, warnings,
            ))
        }
        Err(error) => Ok(ProjectCommandResult::failure(error)),
    }
}

#[tauri::command]
pub fn project_save(
    app: AppHandle,
    request: SaveProjectRequest,
) -> Result<ProjectCommandResult, String> {
    let mut path = request.path;
    if path.is_none() && request.pick_path.unwrap_or(false) {
        let dialog_name = request
            .dialog_name
            .as_deref()
            .unwrap_or(&request.manifest.name);
        match pick_save_path(dialog_name) {
            Ok(picked) => path = Some(picked),
            Err(error) => return Ok(ProjectCommandResult::failure(error)),
        }
    }

    match save_project(path.as_deref(), request.manifest, &app) {
        Ok(opened) => {
            let mut warnings = Vec::new();
            if let Err(error) = touch_recent(&app, &opened) {
                warnings.push(error);
            }
            if !opened.manifest.untitled {
                if let Err(error) = clear_recovery(&app) {
                    warnings.push(error);
                }
            }
            Ok(ProjectCommandResult::success_opened_with_warnings(
                opened, warnings,
            ))
        }
        Err(error) => Ok(ProjectCommandResult::failure(error)),
    }
}

fn autosave_path(request: &SaveProjectRequest) -> Option<&str> {
    if request.manifest.untitled {
        None
    } else {
        request.path.as_deref()
    }
}

#[tauri::command]
pub fn project_autosave(
    app: AppHandle,
    request: SaveProjectRequest,
) -> Result<ProjectCommandResult, String> {
    let path = autosave_path(&request).map(str::to_owned);
    match save_project(path.as_deref(), request.manifest, &app) {
        Ok(opened) => Ok(ProjectCommandResult::success_opened(opened)),
        Err(error) => Ok(ProjectCommandResult::failure(error)),
    }
}

#[tauri::command]
pub fn project_list_recent(app: AppHandle) -> Result<ProjectCommandResult, String> {
    match read_recent(&app) {
        Ok(recent) => Ok(ProjectCommandResult::success_recent(recent)),
        Err(error) => Ok(ProjectCommandResult::failure(error)),
    }
}

#[tauri::command]
pub fn project_remove_recent(
    app: AppHandle,
    request: RemoveRecentRequest,
) -> Result<ProjectCommandResult, String> {
    match remove_recent_entry(&app, &request.path) {
        Ok(recent) => Ok(ProjectCommandResult::success_recent(recent)),
        Err(error) => Ok(ProjectCommandResult::failure(error)),
    }
}

#[tauri::command]
pub async fn project_reveal(request: RevealProjectRequest) -> Result<ProjectCommandResult, String> {
    tauri::async_runtime::spawn_blocking(move || {
        match reveal_project_path(Path::new(&request.path)) {
            Ok(()) => Ok(ProjectCommandResult::success()),
            Err(error) => Ok(ProjectCommandResult::failure(error)),
        }
    })
    .await
    .map_err(|error| format!("project_task_error: {error}"))?
}

#[tauri::command]
pub fn project_recovery_info(app: AppHandle) -> Result<ProjectCommandResult, String> {
    match recovery_info(&app) {
        Ok(info) => Ok(ProjectCommandResult::success_recovery(info)),
        Err(error) => Ok(ProjectCommandResult::failure(error)),
    }
}

#[tauri::command]
pub fn project_recover(app: AppHandle) -> Result<ProjectCommandResult, String> {
    let info = match recovery_info(&app) {
        Ok(info) => info,
        Err(error) => return Ok(ProjectCommandResult::failure(error)),
    };
    let Some(path) = info.path else {
        return Ok(ProjectCommandResult::success_recovery(RecoveryInfo {
            present: false,
            path: None,
            name: None,
            updated_at: None,
        }));
    };
    match open_manifest_at_path(std::path::Path::new(&path)) {
        Ok(opened) => Ok(ProjectCommandResult::success_opened(opened)),
        Err(error) => Ok(ProjectCommandResult::failure(error)),
    }
}

#[tauri::command]
pub fn project_discard_recovery(app: AppHandle) -> Result<ProjectCommandResult, String> {
    match clear_recovery(&app) {
        Ok(()) => Ok(ProjectCommandResult::success_recovery(RecoveryInfo {
            present: false,
            path: None,
            name: None,
            updated_at: None,
        })),
        Err(error) => Ok(ProjectCommandResult::failure(error)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn save_request(untitled: bool, path: Option<&str>) -> SaveProjectRequest {
        SaveProjectRequest {
            path: path.map(str::to_owned),
            manifest: manifest::empty_manifest("Test", untitled),
            pick_path: Some(false),
            dialog_name: None,
        }
    }

    #[test]
    fn untitled_autosave_does_not_promote_recovery_to_a_named_project() {
        let request = save_request(true, Some("/tmp/recovery/untitled.getnative.json"));
        assert_eq!(autosave_path(&request), None);

        let named = save_request(false, Some("/tmp/demo.getnative.json"));
        assert_eq!(autosave_path(&named), Some("/tmp/demo.getnative.json"));
    }

    #[test]
    fn reveal_requires_an_existing_project_file() {
        let missing = std::env::temp_dir().join(format!(
            "getnative_missing_reveal_{}.getnative.json",
            std::process::id()
        ));
        let error = validate_reveal_path(&missing).unwrap_err();
        assert_eq!(error.code, ManifestErrorCode::IoError);
    }
}
