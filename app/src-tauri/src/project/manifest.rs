use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};
use std::collections::HashSet;
use std::path::{Path, PathBuf};

pub const CURRENT_SCHEMA_VERSION: u32 = 1;
pub const PROJECT_FILE_EXTENSION: &str = "getnative.json";

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum SourceKind {
    Still,
    Video,
    Animated,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum SourceState {
    #[default]
    Added,
    Probing,
    Ready,
    Unsupported,
    Missing,
    Error,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct VideoStreamRecord {
    pub index: u32,
    #[serde(default)]
    pub codec_name: Option<String>,
    #[serde(default)]
    pub width: Option<u32>,
    #[serde(default)]
    pub height: Option<u32>,
    #[serde(default)]
    pub duration_seconds: Option<f64>,
    #[serde(default)]
    pub frame_count: Option<u64>,
    #[serde(default)]
    pub time_base_num: Option<i64>,
    #[serde(default)]
    pub time_base_den: Option<i64>,
    #[serde(default)]
    pub frame_rate_num: Option<i64>,
    #[serde(default)]
    pub frame_rate_den: Option<i64>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct SourceRecord {
    pub id: String,
    pub kind: SourceKind,
    pub path: String,
    #[serde(default)]
    pub fingerprint: Option<String>,
    #[serde(default)]
    pub state: SourceState,
    #[serde(default)]
    pub label: Option<String>,
    #[serde(default)]
    pub size_bytes: Option<u64>,
    #[serde(default)]
    pub width: Option<u32>,
    #[serde(default)]
    pub height: Option<u32>,
    #[serde(default)]
    pub duration_seconds: Option<f64>,
    #[serde(default)]
    pub decoder: Option<String>,
    #[serde(default)]
    pub video_streams: Vec<VideoStreamRecord>,
    #[serde(default)]
    pub selected_stream_index: Option<u32>,
    #[serde(default)]
    pub error_code: Option<String>,
    #[serde(default)]
    pub error_detail: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct SampleRecord {
    pub id: String,
    pub source_id: String,
    #[serde(default)]
    pub source_fingerprint: Option<String>,
    #[serde(default)]
    pub label: Option<String>,
    #[serde(default)]
    pub included: bool,
    #[serde(default)]
    pub order: u32,
    #[serde(default)]
    pub frame_index: Option<u64>,
    #[serde(default)]
    pub stream_index: Option<u32>,
    #[serde(default)]
    pub pts: Option<i64>,
    #[serde(default)]
    pub best_effort_timestamp: Option<i64>,
    #[serde(default)]
    pub time_base_num: Option<i64>,
    #[serde(default)]
    pub time_base_den: Option<i64>,
    #[serde(default)]
    pub timestamp_seconds: Option<f64>,
    #[serde(default)]
    pub tags: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct RecipeRecord {
    pub id: String,
    pub name: String,
    /// Schema-1 flag: true for both locked and superseded Recipes.
    #[serde(default)]
    pub locked: bool,
    /// Lifecycle: draft | locked | superseded. Owned by the UI domain layer;
    /// the manifest only round-trips it. When absent it derives from `locked`.
    #[serde(default)]
    pub status: Option<String>,
    #[serde(default)]
    pub revision: u32,
    #[serde(default)]
    pub parent_recipe_id: Option<String>,
    #[serde(default)]
    pub created_at: Option<String>,
    #[serde(default)]
    pub updated_at: Option<String>,
    /// Semantic payload (geometry/kernel/metric/profile/math mode). Required by
    /// the UI before locking; stored as structured JSON, language-neutral.
    #[serde(default)]
    pub geometry: Option<Value>,
    #[serde(default)]
    pub kernel: Option<Value>,
    #[serde(default)]
    pub metric: Option<Value>,
    #[serde(default)]
    pub profile_id: Option<String>,
    #[serde(default)]
    pub math_mode: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct RunGroupRecord {
    pub id: String,
    #[serde(default)]
    pub member_run_ids: Vec<String>,
    #[serde(default)]
    pub group_type: String,
    #[serde(default)]
    pub label: String,
    #[serde(default)]
    pub created_at: String,
    #[serde(default)]
    pub intent_snapshot: Option<Value>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct RunRecord {
    pub id: String,
    #[serde(default)]
    pub run_type: String,
    #[serde(default)]
    pub status: String,
    #[serde(default)]
    pub run_group_id: Option<String>,
    #[serde(default)]
    pub sample_id: Option<String>,
    #[serde(default)]
    pub source_id: Option<String>,
    #[serde(default)]
    pub created_at: String,
    #[serde(default)]
    pub updated_at: String,
    #[serde(default)]
    pub input_snapshot: Option<Value>,
    #[serde(default)]
    pub result: Option<Value>,
    #[serde(default)]
    pub error_code: Option<String>,
    #[serde(default)]
    pub error_message: Option<String>,
    #[serde(default)]
    pub completed: u32,
    #[serde(default)]
    pub total: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct VerificationReviewRecord {
    pub run_id: String,
    #[serde(default)]
    pub tags: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct ProjectManifest {
    pub schema_version: u32,
    pub id: String,
    pub name: String,
    pub created_at: String,
    pub updated_at: String,
    #[serde(default)]
    pub active_recipe_id: Option<String>,
    #[serde(default)]
    pub untitled: bool,
    #[serde(default)]
    pub sources: Vec<SourceRecord>,
    #[serde(default)]
    pub samples: Vec<SampleRecord>,
    #[serde(default)]
    pub recipes: Vec<RecipeRecord>,
    #[serde(default)]
    pub run_groups: Vec<RunGroupRecord>,
    #[serde(default)]
    pub runs: Vec<RunRecord>,
    #[serde(default)]
    pub verification_reviews: Vec<VerificationReviewRecord>,
    #[serde(default)]
    pub ui_state_by_route: Map<String, Value>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum ManifestErrorCode {
    InvalidJson,
    InvalidManifest,
    UnsupportedSchema,
    MissingId,
    MissingName,
    DuplicateId,
    ProjectMismatch,
    PathRejected,
    IoError,
    Cancelled,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct ManifestValidationError {
    pub code: ManifestErrorCode,
    pub message: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct OpenedProject {
    pub manifest: ProjectManifest,
    pub storage_path: Option<String>,
    pub missing_source_ids: Vec<String>,
    pub read_only: bool,
    pub schema_status: String,
}

pub fn now_rfc3339() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let duration = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default();
    let secs = duration.as_secs();
    let nanos = duration.subsec_nanos();
    // Stable UTC timestamp without chrono dependency.
    let days = secs / 86_400;
    let day_secs = secs % 86_400;
    let hour = day_secs / 3_600;
    let minute = (day_secs % 3_600) / 60;
    let second = day_secs % 60;
    let (year, month, day) = civil_from_days(days as i64);
    format!("{year:04}-{month:02}-{day:02}T{hour:02}:{minute:02}:{second:02}.{nanos:09}Z")
}

fn civil_from_days(days: i64) -> (i32, u32, u32) {
    // Howard Hinnant civil_from_days algorithm for Unix epoch day count.
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    (y as i32, m as u32, d as u32)
}

pub fn new_project_id() -> String {
    format!("prj_{}", uuid::Uuid::new_v4().simple())
}

pub fn empty_manifest(name: &str, untitled: bool) -> ProjectManifest {
    let timestamp = now_rfc3339();
    ProjectManifest {
        schema_version: CURRENT_SCHEMA_VERSION,
        id: new_project_id(),
        name: name.to_owned(),
        created_at: timestamp.clone(),
        updated_at: timestamp,
        active_recipe_id: None,
        untitled,
        sources: Vec::new(),
        samples: Vec::new(),
        recipes: Vec::new(),
        run_groups: Vec::new(),
        runs: Vec::new(),
        verification_reviews: Vec::new(),
        ui_state_by_route: Map::new(),
    }
}

pub fn validate_manifest(manifest: &ProjectManifest) -> Result<(), ManifestValidationError> {
    if manifest.schema_version == 0 {
        return Err(ManifestValidationError {
            code: ManifestErrorCode::InvalidManifest,
            message: "schema_version must be >= 1".to_owned(),
        });
    }
    if manifest.schema_version > CURRENT_SCHEMA_VERSION {
        return Err(ManifestValidationError {
            code: ManifestErrorCode::UnsupportedSchema,
            message: format!(
                "schema_version {} is newer than supported {}",
                manifest.schema_version, CURRENT_SCHEMA_VERSION
            ),
        });
    }
    if manifest.schema_version != CURRENT_SCHEMA_VERSION {
        return Err(ManifestValidationError {
            code: ManifestErrorCode::InvalidManifest,
            message: format!(
                "schema_version {} is not supported for write validation",
                manifest.schema_version
            ),
        });
    }
    if manifest.id.trim().is_empty() {
        return Err(ManifestValidationError {
            code: ManifestErrorCode::MissingId,
            message: "project id is required".to_owned(),
        });
    }
    if manifest.name.trim().is_empty() {
        return Err(ManifestValidationError {
            code: ManifestErrorCode::MissingName,
            message: "project name is required".to_owned(),
        });
    }

    let mut ids = HashSet::new();
    let mut source_ids = HashSet::new();
    let mut recipe_ids = HashSet::new();
    let mut run_ids = HashSet::new();
    if !ids.insert(manifest.id.clone()) {
        return Err(duplicate_id(&manifest.id));
    }
    for source in &manifest.sources {
        if source.id.trim().is_empty() || !ids.insert(source.id.clone()) {
            return Err(duplicate_id(&source.id));
        }
        source_ids.insert(source.id.clone());
        if source.path.trim().is_empty() {
            return Err(ManifestValidationError {
                code: ManifestErrorCode::InvalidManifest,
                message: format!("source {} is missing a path", source.id),
            });
        }
    }
    for sample in &manifest.samples {
        if sample.id.trim().is_empty() || !ids.insert(sample.id.clone()) {
            return Err(duplicate_id(&sample.id));
        }
        if !source_ids.contains(&sample.source_id) {
            return Err(ManifestValidationError {
                code: ManifestErrorCode::InvalidManifest,
                message: format!(
                    "sample {} references missing source {}",
                    sample.id, sample.source_id
                ),
            });
        }
    }
    for recipe in &manifest.recipes {
        if recipe.id.trim().is_empty() || !ids.insert(recipe.id.clone()) {
            return Err(duplicate_id(&recipe.id));
        }
        recipe_ids.insert(recipe.id.clone());
    }
    for group in &manifest.run_groups {
        if group.id.trim().is_empty() || !ids.insert(group.id.clone()) {
            return Err(duplicate_id(&group.id));
        }
    }
    for run in &manifest.runs {
        if run.id.trim().is_empty() || !ids.insert(run.id.clone()) {
            return Err(duplicate_id(&run.id));
        }
        run_ids.insert(run.id.clone());
    }

    if let Some(active) = &manifest.active_recipe_id {
        if !recipe_ids.contains(active) {
            return Err(ManifestValidationError {
                code: ManifestErrorCode::InvalidManifest,
                message: format!("active_recipe_id {active} is not present in recipes"),
            });
        }
        if !manifest
            .recipes
            .iter()
            .any(|recipe| recipe.id == *active && recipe.locked)
        {
            return Err(ManifestValidationError {
                code: ManifestErrorCode::InvalidManifest,
                message: format!("active recipe {active} must be locked"),
            });
        }
    }

    for group in &manifest.run_groups {
        for run_id in &group.member_run_ids {
            if !run_ids.contains(run_id) {
                return Err(ManifestValidationError {
                    code: ManifestErrorCode::InvalidManifest,
                    message: format!("run group {} references missing run {}", group.id, run_id),
                });
            }
        }
    }

    let mut reviewed_run_ids = HashSet::new();
    for review in &manifest.verification_reviews {
        if !run_ids.contains(&review.run_id) {
            return Err(ManifestValidationError {
                code: ManifestErrorCode::InvalidManifest,
                message: format!("review references missing run {}", review.run_id),
            });
        }
        if !reviewed_run_ids.insert(review.run_id.clone()) {
            return Err(ManifestValidationError {
                code: ManifestErrorCode::InvalidManifest,
                message: format!("run {} has more than one review record", review.run_id),
            });
        }
    }

    Ok(())
}

fn duplicate_id(id: &str) -> ManifestValidationError {
    ManifestValidationError {
        code: ManifestErrorCode::DuplicateId,
        message: format!("duplicate or empty id: {id}"),
    }
}

pub fn parse_manifest_bytes(bytes: &[u8]) -> Result<ProjectManifest, ManifestValidationError> {
    let value: Value = serde_json::from_slice(bytes).map_err(|error| ManifestValidationError {
        code: ManifestErrorCode::InvalidJson,
        message: format!("invalid JSON: {error}"),
    })?;
    parse_manifest_value(value)
}

pub fn parse_newer_manifest_read_only(
    bytes: &[u8],
) -> Result<ProjectManifest, ManifestValidationError> {
    let value: Value = serde_json::from_slice(bytes).map_err(|error| ManifestValidationError {
        code: ManifestErrorCode::InvalidJson,
        message: format!("invalid JSON: {error}"),
    })?;
    let schema_version = value
        .get("schema_version")
        .and_then(Value::as_u64)
        .and_then(|version| u32::try_from(version).ok())
        .ok_or_else(|| ManifestValidationError {
            code: ManifestErrorCode::InvalidManifest,
            message: "schema_version is required and must fit in u32".to_owned(),
        })?;
    if schema_version <= CURRENT_SCHEMA_VERSION {
        return Err(ManifestValidationError {
            code: ManifestErrorCode::InvalidManifest,
            message: "read-only newer-schema parser requires a newer schema".to_owned(),
        });
    }

    let manifest: ProjectManifest =
        serde_json::from_value(value).map_err(|error| ManifestValidationError {
            code: ManifestErrorCode::InvalidManifest,
            message: format!("newer manifest is not backward-readable: {error}"),
        })?;
    let mut compatible_shape = manifest.clone();
    compatible_shape.schema_version = CURRENT_SCHEMA_VERSION;
    validate_manifest(&compatible_shape)?;
    Ok(manifest)
}

pub fn parse_manifest_value(value: Value) -> Result<ProjectManifest, ManifestValidationError> {
    let schema_version = value
        .get("schema_version")
        .and_then(Value::as_u64)
        .and_then(|version| u32::try_from(version).ok())
        .ok_or_else(|| ManifestValidationError {
            code: ManifestErrorCode::InvalidManifest,
            message: "schema_version is required and must fit in u32".to_owned(),
        })?;

    if schema_version > CURRENT_SCHEMA_VERSION {
        return Err(ManifestValidationError {
            code: ManifestErrorCode::UnsupportedSchema,
            message: format!(
                "schema_version {schema_version} is newer than supported {CURRENT_SCHEMA_VERSION}"
            ),
        });
    }

    let manifest: ProjectManifest =
        serde_json::from_value(value).map_err(|error| ManifestValidationError {
            code: ManifestErrorCode::InvalidManifest,
            message: format!("manifest fields are invalid: {error}"),
        })?;
    validate_manifest(&manifest)?;
    Ok(manifest)
}

pub fn serialize_manifest(manifest: &ProjectManifest) -> Result<Vec<u8>, ManifestValidationError> {
    validate_manifest(manifest)?;
    let mut bytes =
        serde_json::to_vec_pretty(manifest).map_err(|error| ManifestValidationError {
            code: ManifestErrorCode::InvalidManifest,
            message: format!("failed to serialize manifest: {error}"),
        })?;
    bytes.push(b'\n');
    Ok(bytes)
}

#[cfg(test)]
pub fn missing_source_ids(manifest: &ProjectManifest) -> Vec<String> {
    manifest
        .sources
        .iter()
        .filter(|source| {
            let path = Path::new(&source.path);
            !path.is_file()
        })
        .map(|source| source.id.clone())
        .collect()
}

pub fn mark_missing_sources(manifest: &mut ProjectManifest) -> Vec<String> {
    let mut missing = Vec::new();
    for source in &mut manifest.sources {
        if !Path::new(&source.path).is_file() {
            source.state = SourceState::Missing;
            missing.push(source.id.clone());
        } else if source.state == SourceState::Missing {
            source.state = SourceState::Added;
        }
    }
    missing
}

pub fn normalize_project_path(path: &Path) -> Result<PathBuf, ManifestValidationError> {
    if path.as_os_str().is_empty() {
        return Err(ManifestValidationError {
            code: ManifestErrorCode::PathRejected,
            message: "project path is empty".to_owned(),
        });
    }
    let absolute = if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir()
            .map_err(|error| ManifestValidationError {
                code: ManifestErrorCode::IoError,
                message: format!("failed to resolve current directory: {error}"),
            })?
            .join(path)
    };
    if let Some(name) = absolute.file_name().and_then(|name| name.to_str()) {
        if name.trim().is_empty() {
            return Err(ManifestValidationError {
                code: ManifestErrorCode::PathRejected,
                message: "project path has an empty file name".to_owned(),
            });
        }
    } else {
        return Err(ManifestValidationError {
            code: ManifestErrorCode::PathRejected,
            message: "project path is not a file path".to_owned(),
        });
    }
    Ok(absolute)
}

pub fn ensure_project_extension(path: PathBuf) -> PathBuf {
    let file_name = path
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or("project")
        .to_owned();
    if file_name.ends_with(PROJECT_FILE_EXTENSION) {
        return path;
    }
    let mut with_ext = path;
    let new_name = format!("{file_name}.{PROJECT_FILE_EXTENSION}");
    with_ext.set_file_name(new_name);
    with_ext
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn empty_manifest_validates() {
        let manifest = empty_manifest("Demo", false);
        assert!(validate_manifest(&manifest).is_ok());
    }

    #[test]
    fn newer_schema_is_rejected_without_overwrite() {
        let value = json!({
            "schema_version": 99,
            "id": "x",
            "name": "Future",
            "created_at": "2026-01-01T00:00:00Z",
            "updated_at": "2026-01-01T00:00:00Z"
        });
        let error = parse_manifest_value(value).unwrap_err();
        assert_eq!(error.code, ManifestErrorCode::UnsupportedSchema);
    }

    #[test]
    fn backward_readable_newer_schema_can_be_inspected_without_rewriting_version() {
        let mut value = serde_json::to_value(empty_manifest("Future", false)).unwrap();
        value["schema_version"] = json!(99);
        value["future_field"] = json!({ "preserved_on_disk": true });
        let bytes = serde_json::to_vec(&value).unwrap();
        let manifest = parse_newer_manifest_read_only(&bytes).unwrap();
        assert_eq!(manifest.schema_version, 99);
        assert_eq!(manifest.name, "Future");
    }

    #[test]
    fn invalid_json_is_actionable() {
        let error = parse_manifest_bytes(b"{not-json").unwrap_err();
        assert_eq!(error.code, ManifestErrorCode::InvalidJson);
    }

    #[test]
    fn active_recipe_must_exist() {
        let mut manifest = empty_manifest("Demo", false);
        manifest.active_recipe_id = Some("missing".to_owned());
        assert!(validate_manifest(&manifest).is_err());
    }

    #[test]
    fn active_recipe_must_be_locked() {
        let mut manifest = empty_manifest("Demo", false);
        manifest.recipes.push(RecipeRecord {
            id: "recipe_1".to_owned(),
            name: "Candidate".to_owned(),
            locked: false,
            status: None,
            revision: 1,
            parent_recipe_id: None,
            created_at: None,
            updated_at: None,
            geometry: None,
            kernel: None,
            metric: None,
            profile_id: None,
            math_mode: None,
        });
        manifest.active_recipe_id = Some("recipe_1".to_owned());
        let error = validate_manifest(&manifest).unwrap_err();
        assert!(error.message.contains("must be locked"));
    }

    #[test]
    fn references_must_resolve() {
        let mut manifest = empty_manifest("Demo", false);
        manifest.samples.push(SampleRecord {
            id: "sample_1".to_owned(),
            source_id: "missing_source".to_owned(),
            source_fingerprint: None,
            label: None,
            included: true,
            order: 0,
            frame_index: Some(10),
            stream_index: None,
            pts: None,
            best_effort_timestamp: None,
            time_base_num: None,
            time_base_den: None,
            timestamp_seconds: None,
            tags: Vec::new(),
        });
        assert!(validate_manifest(&manifest)
            .unwrap_err()
            .message
            .contains("missing source"));

        manifest.samples.clear();
        manifest.run_groups.push(RunGroupRecord {
            id: "group_1".to_owned(),
            member_run_ids: vec!["missing_run".to_owned()],
            group_type: String::new(),
            label: String::new(),
            created_at: String::new(),
            intent_snapshot: None,
        });
        assert!(validate_manifest(&manifest)
            .unwrap_err()
            .message
            .contains("missing run"));

        manifest.run_groups.clear();
        manifest
            .verification_reviews
            .push(VerificationReviewRecord {
                run_id: "missing_run".to_owned(),
                tags: Vec::new(),
            });
        assert!(validate_manifest(&manifest)
            .unwrap_err()
            .message
            .contains("missing run"));
    }

    #[test]
    fn project_ids_are_uuid_based_and_unique() {
        let first = new_project_id();
        let second = new_project_id();
        assert!(first.starts_with("prj_"));
        assert_eq!(first.len(), 36);
        assert_ne!(first, second);
    }

    #[test]
    fn missing_source_paths_are_reported() {
        let mut manifest = empty_manifest("Demo", false);
        manifest.sources.push(SourceRecord {
            id: "src_1".to_owned(),
            kind: SourceKind::Still,
            path: "/definitely/missing/media.png".to_owned(),
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
        let missing = missing_source_ids(&manifest);
        assert_eq!(missing, vec!["src_1".to_owned()]);
        assert!(validate_manifest(&manifest).is_ok());
    }

    #[test]
    fn ensure_extension_appends_once() {
        let path = ensure_project_extension(PathBuf::from("/tmp/demo"));
        assert!(path
            .file_name()
            .unwrap()
            .to_str()
            .unwrap()
            .ends_with(PROJECT_FILE_EXTENSION));
        let again = ensure_project_extension(path.clone());
        assert_eq!(path, again);
    }
}
