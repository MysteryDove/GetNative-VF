use super::manifest::{
    empty_manifest, ensure_project_extension, mark_missing_sources, normalize_project_path,
    parse_manifest_bytes, parse_newer_manifest_read_only, serialize_manifest, ManifestErrorCode,
    ManifestValidationError, OpenedProject, ProjectManifest, CURRENT_SCHEMA_VERSION,
    PROJECT_FILE_EXTENSION,
};
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};
use tauri::{AppHandle, Manager};

const RECENT_LIMIT: usize = 20;
const RECENT_FILE: &str = "recent_projects.json";
const RECOVERY_DIR: &str = "recovery";
const RECOVERY_FILE: &str = "untitled.getnative.json";

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct RecentProjectEntry {
    pub path: String,
    pub id: String,
    pub name: String,
    pub last_opened_at: String,
    pub source_count: u32,
    pub active_recipe_name: Option<String>,
    pub has_missing_media: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct RecoveryInfo {
    pub present: bool,
    pub path: Option<String>,
    pub name: Option<String>,
    pub updated_at: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct ProjectCommandResult {
    pub ok: bool,
    pub opened: Option<OpenedProject>,
    pub recent: Option<Vec<RecentProjectEntry>>,
    pub recovery: Option<RecoveryInfo>,
    pub error: Option<ManifestValidationError>,
    pub warnings: Vec<ManifestValidationError>,
}

impl ProjectCommandResult {
    pub fn success() -> Self {
        Self {
            ok: true,
            opened: None,
            recent: None,
            recovery: None,
            error: None,
            warnings: Vec::new(),
        }
    }

    pub fn success_opened(opened: OpenedProject) -> Self {
        Self::success_opened_with_warnings(opened, Vec::new())
    }

    pub fn success_opened_with_warnings(
        opened: OpenedProject,
        warnings: Vec<ManifestValidationError>,
    ) -> Self {
        Self {
            ok: true,
            opened: Some(opened),
            recent: None,
            recovery: None,
            error: None,
            warnings,
        }
    }

    pub fn success_recent(recent: Vec<RecentProjectEntry>) -> Self {
        Self {
            ok: true,
            opened: None,
            recent: Some(recent),
            recovery: None,
            error: None,
            warnings: Vec::new(),
        }
    }

    pub fn success_recovery(recovery: RecoveryInfo) -> Self {
        Self {
            ok: true,
            opened: None,
            recent: None,
            recovery: Some(recovery),
            error: None,
            warnings: Vec::new(),
        }
    }

    pub fn failure(error: ManifestValidationError) -> Self {
        Self {
            ok: false,
            opened: None,
            recent: None,
            recovery: None,
            error: Some(error),
            warnings: Vec::new(),
        }
    }
}

pub fn app_data_root(app: &AppHandle) -> Result<PathBuf, ManifestValidationError> {
    app.path()
        .app_data_dir()
        .map_err(|error| ManifestValidationError {
            code: ManifestErrorCode::IoError,
            message: format!("failed to resolve app data directory: {error}"),
        })
}

pub fn recent_list_path(app: &AppHandle) -> Result<PathBuf, ManifestValidationError> {
    Ok(app_data_root(app)?.join(RECENT_FILE))
}

pub fn recovery_path(app: &AppHandle) -> Result<PathBuf, ManifestValidationError> {
    Ok(app_data_root(app)?.join(RECOVERY_DIR).join(RECOVERY_FILE))
}

fn ensure_parent(path: &Path) -> Result<(), ManifestValidationError> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|error| ManifestValidationError {
            code: ManifestErrorCode::IoError,
            message: format!("failed to create directory {}: {error}", parent.display()),
        })?;
    }
    Ok(())
}

fn write_json_atomically(
    path: &Path,
    bytes: &[u8],
    subject: &str,
) -> Result<(), ManifestValidationError> {
    crate::atomic_file::write_bytes(path, bytes).map_err(|error| ManifestValidationError {
        code: ManifestErrorCode::IoError,
        message: format!("failed to write {subject}: {error}"),
    })
}

pub fn read_recent(app: &AppHandle) -> Result<Vec<RecentProjectEntry>, ManifestValidationError> {
    let path = recent_list_path(app)?;
    if !path.is_file() {
        return Ok(Vec::new());
    }
    let bytes = fs::read(&path).map_err(|error| ManifestValidationError {
        code: ManifestErrorCode::IoError,
        message: format!("failed to read recent list: {error}"),
    })?;
    serde_json::from_slice(&bytes).map_err(|error| ManifestValidationError {
        code: ManifestErrorCode::InvalidJson,
        message: format!("recent list is invalid: {error}"),
    })
}

pub fn write_recent(
    app: &AppHandle,
    entries: &[RecentProjectEntry],
) -> Result<(), ManifestValidationError> {
    let path = recent_list_path(app)?;
    ensure_parent(&path)?;
    let bytes = serde_json::to_vec_pretty(entries).map_err(|error| ManifestValidationError {
        code: ManifestErrorCode::IoError,
        message: format!("failed to serialize recent list: {error}"),
    })?;
    write_json_atomically(&path, &bytes, "recent list")
}

pub fn touch_recent(
    app: &AppHandle,
    opened: &OpenedProject,
) -> Result<Vec<RecentProjectEntry>, ManifestValidationError> {
    let Some(storage_path) = opened.storage_path.as_ref() else {
        return read_recent(app);
    };
    if opened.manifest.untitled {
        return read_recent(app);
    }

    let mut entries = read_recent(app)?;
    entries.retain(|entry| entry.path != *storage_path);
    let active_recipe_name = opened.manifest.active_recipe_id.as_ref().and_then(|id| {
        opened
            .manifest
            .recipes
            .iter()
            .find(|recipe| recipe.id == *id)
            .map(|recipe| recipe.name.clone())
    });
    entries.insert(
        0,
        RecentProjectEntry {
            path: storage_path.clone(),
            id: opened.manifest.id.clone(),
            name: opened.manifest.name.clone(),
            last_opened_at: super::manifest::now_rfc3339(),
            source_count: opened.manifest.sources.len() as u32,
            active_recipe_name,
            has_missing_media: !opened.missing_source_ids.is_empty(),
        },
    );
    entries.truncate(RECENT_LIMIT);
    write_recent(app, &entries)?;
    Ok(entries)
}

pub fn remove_recent_entry(
    app: &AppHandle,
    path: &str,
) -> Result<Vec<RecentProjectEntry>, ManifestValidationError> {
    let mut entries = read_recent(app)?;
    entries.retain(|entry| entry.path != path);
    write_recent(app, &entries)?;
    Ok(entries)
}

pub fn write_manifest_to_path(
    path: &Path,
    manifest: &ProjectManifest,
) -> Result<(), ManifestValidationError> {
    let normalized = normalize_project_path(path)?;
    let bytes = serialize_manifest(manifest)?;

    // A save may update only the same valid Project. Invalid, unsupported, or
    // unrelated files remain untouched.
    if normalized.is_file() {
        let existing = fs::read(&normalized).map_err(|error| ManifestValidationError {
            code: ManifestErrorCode::IoError,
            message: format!("failed to read existing project: {error}"),
        })?;
        let existing_manifest = parse_manifest_bytes(&existing)?;
        if existing_manifest.id != manifest.id {
            return Err(ManifestValidationError {
                code: ManifestErrorCode::ProjectMismatch,
                message: format!(
                    "refusing to replace project {} with {} at {}",
                    existing_manifest.id,
                    manifest.id,
                    normalized.display()
                ),
            });
        }
    }
    ensure_parent(&normalized)?;
    write_json_atomically(&normalized, &bytes, "project file")
}

pub fn open_manifest_at_path(path: &Path) -> Result<OpenedProject, ManifestValidationError> {
    let normalized = normalize_project_path(path)?;
    if !normalized.is_file() {
        return Err(ManifestValidationError {
            code: ManifestErrorCode::IoError,
            message: format!("project file not found: {}", normalized.display()),
        });
    }
    let bytes = fs::read(&normalized).map_err(|error| ManifestValidationError {
        code: ManifestErrorCode::IoError,
        message: format!("failed to read project: {error}"),
    })?;

    match parse_manifest_bytes(&bytes) {
        Ok(mut manifest) => {
            let missing = mark_missing_sources(&mut manifest);
            Ok(OpenedProject {
                manifest,
                storage_path: Some(normalized.display().to_string()),
                missing_source_ids: missing,
                read_only: false,
                schema_status: "supported".to_owned(),
            })
        }
        Err(error) if error.code == ManifestErrorCode::UnsupportedSchema => {
            let mut manifest = parse_newer_manifest_read_only(&bytes)?;
            let missing = mark_missing_sources(&mut manifest);
            Ok(OpenedProject {
                manifest,
                storage_path: Some(normalized.display().to_string()),
                missing_source_ids: missing,
                read_only: true,
                schema_status: "newer_read_only".to_owned(),
            })
        }
        Err(error) => Err(error),
    }
}

pub fn create_project_at_path(
    path: &Path,
    name: &str,
) -> Result<OpenedProject, ManifestValidationError> {
    let path = ensure_project_extension(normalize_project_path(path)?);
    if path.is_file() {
        return Err(ManifestValidationError {
            code: ManifestErrorCode::PathRejected,
            message: format!("project file already exists: {}", path.display()),
        });
    }
    let mut manifest = empty_manifest(name, false);
    manifest.updated_at = super::manifest::now_rfc3339();
    write_manifest_to_path(&path, &manifest)?;
    Ok(OpenedProject {
        storage_path: Some(path.display().to_string()),
        missing_source_ids: Vec::new(),
        read_only: false,
        schema_status: "supported".to_owned(),
        manifest,
    })
}

pub fn create_untitled_project(app: &AppHandle) -> Result<OpenedProject, ManifestValidationError> {
    create_untitled_at_path(&recovery_path(app)?)
}

/// The recovery path is a single scratch slot, not a named project. A new
/// untitled session must replace whatever is already there — even when the
/// previous recovery file belongs to a different project id. Reusing
/// `write_manifest_to_path` would return `project_mismatch`. Write atomically
/// so a failed create leaves the previous recovery file intact.
fn create_untitled_at_path(path: &Path) -> Result<OpenedProject, ManifestValidationError> {
    let manifest = empty_manifest("Untitled", true);
    let normalized = normalize_project_path(path)?;
    let bytes = serialize_manifest(&manifest)?;
    ensure_parent(&normalized)?;
    write_json_atomically(&normalized, &bytes, "recovery project")?;
    Ok(OpenedProject {
        storage_path: Some(normalized.display().to_string()),
        missing_source_ids: Vec::new(),
        read_only: false,
        schema_status: "supported".to_owned(),
        manifest,
    })
}

fn project_name_from_path(path: &Path) -> Option<String> {
    let file_name = path.file_name()?.to_str()?;
    let suffix = format!(".{PROJECT_FILE_EXTENSION}");
    let name = file_name.strip_suffix(&suffix).unwrap_or(file_name).trim();
    (!name.is_empty()).then(|| name.to_owned())
}

pub fn save_project(
    path: Option<&str>,
    mut manifest: ProjectManifest,
    app: &AppHandle,
) -> Result<OpenedProject, ManifestValidationError> {
    if manifest.schema_version != CURRENT_SCHEMA_VERSION {
        let code = if manifest.schema_version > CURRENT_SCHEMA_VERSION {
            ManifestErrorCode::UnsupportedSchema
        } else {
            ManifestErrorCode::InvalidManifest
        };
        return Err(ManifestValidationError {
            code,
            message: format!(
                "refusing to save schema_version {}; expected {}",
                manifest.schema_version, CURRENT_SCHEMA_VERSION
            ),
        });
    }
    manifest.updated_at = super::manifest::now_rfc3339();

    let target = if let Some(path) = path {
        let target = ensure_project_extension(normalize_project_path(Path::new(path))?);
        if manifest.untitled {
            if let Some(name) = project_name_from_path(&target) {
                manifest.name = name;
            }
        }
        manifest.untitled = false;
        target
    } else if manifest.untitled {
        recovery_path(app)?
    } else {
        return Err(ManifestValidationError {
            code: ManifestErrorCode::PathRejected,
            message: "save path is required for named projects".to_owned(),
        });
    };

    write_manifest_to_path(&target, &manifest)?;
    let missing = mark_missing_sources(&mut manifest);
    Ok(OpenedProject {
        storage_path: Some(target.display().to_string()),
        missing_source_ids: missing,
        read_only: false,
        schema_status: "supported".to_owned(),
        manifest,
    })
}

pub fn recovery_info(app: &AppHandle) -> Result<RecoveryInfo, ManifestValidationError> {
    let path = recovery_path(app)?;
    if !path.is_file() {
        return Ok(RecoveryInfo {
            present: false,
            path: None,
            name: None,
            updated_at: None,
        });
    }
    match open_manifest_at_path(&path) {
        Ok(opened) => Ok(RecoveryInfo {
            present: true,
            path: opened.storage_path,
            name: (!opened.manifest.untitled).then_some(opened.manifest.name),
            updated_at: Some(opened.manifest.updated_at),
        }),
        Err(_) => Ok(RecoveryInfo {
            present: true,
            path: Some(path.display().to_string()),
            name: None,
            updated_at: None,
        }),
    }
}

pub fn clear_recovery(app: &AppHandle) -> Result<(), ManifestValidationError> {
    let path = recovery_path(app)?;
    if path.is_file() {
        fs::remove_file(&path).map_err(|error| ManifestValidationError {
            code: ManifestErrorCode::IoError,
            message: format!("failed to clear recovery project: {error}"),
        })?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::project::manifest::{empty_manifest, SourceKind, SourceRecord, SourceState};
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_dir() -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        // Include thread id so parallel tests cannot collide on the same nanosecond.
        let thread = format!("{:?}", std::thread::current().id()).replace(['(', ')'], "");
        let dir = std::env::temp_dir().join(format!("getnative_project_test_{thread}_{nanos}"));
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn create_open_save_round_trip() {
        let dir = temp_dir();
        let path = dir.join("demo.getnative.json");
        let opened = create_project_at_path(&path, "Demo").unwrap();
        assert_eq!(opened.manifest.name, "Demo");
        assert!(path.is_file());

        let reopened = open_manifest_at_path(&path).unwrap();
        assert_eq!(reopened.manifest.id, opened.manifest.id);

        let mut updated = reopened.manifest.clone();
        updated.name = "Demo Renamed".to_owned();
        write_manifest_to_path(&path, &updated).unwrap();
        let again = open_manifest_at_path(&path).unwrap();
        assert_eq!(again.manifest.name, "Demo Renamed");
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn create_open_save_round_trip_in_unicode_directory() {
        let root = temp_dir();
        let dir = root.join("\u{4e2d}\u{6587}-\u{65e5}\u{672c}\u{8a9e}-\u{d55c}\u{ae00}");
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join("\u{9879}\u{76ee}-\u{5922}.getnative.json");

        let opened = create_project_at_path(&path, "Unicode").unwrap();
        let reopened = open_manifest_at_path(&path).unwrap();

        assert_eq!(reopened.manifest.id, opened.manifest.id);
        assert_eq!(reopened.storage_path.as_deref(), path.to_str());
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn missing_media_does_not_block_open() {
        let dir = temp_dir();
        let path = dir.join("missing-media.getnative.json");
        let mut opened = create_project_at_path(&path, "Missing").unwrap();
        opened.manifest.sources.push(SourceRecord {
            id: "src_missing".to_owned(),
            kind: SourceKind::Video,
            path: dir.join("nope.mkv").display().to_string(),
            fingerprint: None,
            state: SourceState::Ready,
            label: None,
            size_bytes: None,
            width: None,
            height: None,
            duration_seconds: None,
            decoder: None,
            video_streams: Vec::new(),
            selected_stream_index: None,
            error_code: None,
            error_detail: None,
        });
        write_manifest_to_path(&path, &opened.manifest).unwrap();
        let reopened = open_manifest_at_path(&path).unwrap();
        assert_eq!(reopened.missing_source_ids, vec!["src_missing".to_owned()]);
        assert_eq!(reopened.manifest.sources[0].state, SourceState::Missing);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn refuses_to_overwrite_newer_schema() {
        let dir = temp_dir();
        let path = dir.join("future.getnative.json");
        fs::write(
            &path,
            r#"{
  "schema_version": 99,
  "id": "future",
  "name": "Future",
  "created_at": "2026-01-01T00:00:00Z",
  "updated_at": "2026-01-01T00:00:00Z"
}
"#,
        )
        .unwrap();
        let manifest = empty_manifest("Overwrite", false);
        let error = write_manifest_to_path(&path, &manifest).unwrap_err();
        assert_eq!(error.code, ManifestErrorCode::UnsupportedSchema);
        let original = fs::read_to_string(&path).unwrap();
        assert!(original.contains("\"schema_version\": 99"));
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn refuses_to_overwrite_invalid_json_or_another_project() {
        let dir = temp_dir();
        let path = dir.join("guarded.getnative.json");
        fs::write(&path, b"{not-json").unwrap();
        let manifest = empty_manifest("Incoming", false);
        assert_eq!(
            write_manifest_to_path(&path, &manifest).unwrap_err().code,
            ManifestErrorCode::InvalidJson
        );
        assert_eq!(fs::read(&path).unwrap(), b"{not-json");

        let existing = empty_manifest("Existing", false);
        fs::write(&path, serialize_manifest(&existing).unwrap()).unwrap();
        assert_eq!(
            write_manifest_to_path(&path, &manifest).unwrap_err().code,
            ManifestErrorCode::ProjectMismatch
        );
        let unchanged = open_manifest_at_path(&path).unwrap();
        assert_eq!(unchanged.manifest.id, existing.id);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn existing_project_is_replaced_without_leaving_a_temp_file() {
        let dir = temp_dir();
        let path = dir.join("atomic.getnative.json");
        let manifest = empty_manifest("Before", false);
        write_manifest_to_path(&path, &manifest).unwrap();

        let mut updated = manifest;
        updated.name = "After".to_owned();
        write_manifest_to_path(&path, &updated).unwrap();

        assert_eq!(open_manifest_at_path(&path).unwrap().manifest.name, "After");
        assert_eq!(fs::read_dir(&dir).unwrap().count(), 1);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn open_rejects_invalid_manifest_without_mutation() {
        let dir = temp_dir();
        let path = dir.join("bad.getnative.json");
        let original = r#"{"schema_version":1,"id":"","name":"Bad"}"#;
        fs::write(&path, original).unwrap();
        assert!(open_manifest_at_path(&path).is_err());
        assert_eq!(fs::read_to_string(&path).unwrap(), original);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn opens_backward_readable_newer_schema_as_read_only() {
        let dir = temp_dir();
        let path = dir.join("future-readable.getnative.json");
        let mut value = serde_json::to_value(empty_manifest("Future", false)).unwrap();
        value["schema_version"] = serde_json::json!(3);
        value["future_field"] = serde_json::json!(true);
        let original = serde_json::to_vec_pretty(&value).unwrap();
        fs::write(&path, &original).unwrap();

        let opened = open_manifest_at_path(&path).unwrap();
        assert!(opened.read_only);
        assert_eq!(opened.schema_status, "newer_read_only");
        assert_eq!(opened.manifest.schema_version, 3);
        assert_eq!(fs::read(&path).unwrap(), original);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn saved_untitled_project_uses_the_selected_file_name() {
        assert_eq!(
            project_name_from_path(Path::new("/tmp/示例.getnative.json")),
            Some("示例".to_owned())
        );
        assert_eq!(
            project_name_from_path(Path::new("/tmp/Demo.getnative.json")),
            Some("Demo".to_owned())
        );
    }

    #[test]
    fn untitled_create_replaces_existing_recovery_with_a_different_id() {
        let dir = temp_dir();
        let path = dir.join("untitled.getnative.json");
        let existing = empty_manifest("Untitled", true);
        write_manifest_to_path(&path, &existing).unwrap();

        let opened = create_untitled_at_path(&path).unwrap();
        assert_ne!(opened.manifest.id, existing.id);
        assert!(opened.manifest.untitled);
        assert_eq!(
            open_manifest_at_path(&path).unwrap().manifest.id,
            opened.manifest.id
        );
        let _ = fs::remove_dir_all(dir);
    }
}
