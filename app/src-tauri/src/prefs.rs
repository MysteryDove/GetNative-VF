use serde::{Deserialize, Serialize};
use std::fs;
use std::path::PathBuf;
use tauri::{AppHandle, Manager};

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq, Eq)]
pub enum AppLanguage {
    #[serde(rename = "zh-CN")]
    #[default]
    ZhCn,
    #[serde(rename = "en")]
    En,
}

/// Theme selection: `system` follows the OS appearance, the other two pin the
/// UI regardless of the OS setting.
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq, Eq)]
pub enum AppTheme {
    #[serde(rename = "system")]
    #[default]
    System,
    #[serde(rename = "light")]
    Light,
    #[serde(rename = "dark")]
    Dark,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct AppPreferences {
    pub language: AppLanguage,
    #[serde(default)]
    pub axis_plan_cache_dir: Option<String>,
    #[serde(default)]
    pub theme: AppTheme,
}

impl Default for AppPreferences {
    fn default() -> Self {
        Self {
            language: AppLanguage::ZhCn,
            axis_plan_cache_dir: None,
            theme: AppTheme::System,
        }
    }
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SetLanguageRequest {
    pub language: AppLanguage,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SetAxisPlanCacheDirRequest {
    pub path: Option<String>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SetThemeRequest {
    pub theme: AppTheme,
}

fn preferences_path(app: &AppHandle) -> Result<PathBuf, String> {
    let dir = app
        .path()
        .app_config_dir()
        .map_err(|error| format!("failed to resolve app config directory: {error}"))?;
    Ok(dir.join("preferences.json"))
}

pub fn load_preferences(app: &AppHandle) -> Result<AppPreferences, String> {
    let path = preferences_path(app)?;
    if !path.is_file() {
        return Ok(AppPreferences::default());
    }
    let bytes = fs::read(&path).map_err(|error| format!("failed to read preferences: {error}"))?;
    serde_json::from_slice(&bytes).map_err(|error| format!("preferences are invalid: {error}"))
}

pub fn save_preferences(app: &AppHandle, prefs: &AppPreferences) -> Result<(), String> {
    let path = preferences_path(app)?;
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)
            .map_err(|error| format!("failed to create config directory: {error}"))?;
    }
    let bytes = serde_json::to_vec_pretty(prefs)
        .map_err(|error| format!("failed to serialize preferences: {error}"))?;
    crate::atomic_file::write_bytes(&path, &bytes)
        .map_err(|error| format!("failed to write preferences: {error}"))
}

#[tauri::command]
pub fn app_get_preferences(app: AppHandle) -> Result<AppPreferences, String> {
    load_preferences(&app)
}

#[tauri::command]
pub fn app_set_language(
    app: AppHandle,
    request: SetLanguageRequest,
) -> Result<AppPreferences, String> {
    // Preserve unrelated preferences (e.g. axis_plan_cache_dir) across a
    // language change.
    let mut prefs = load_preferences(&app)?;
    prefs.language = request.language;
    save_preferences(&app, &prefs)?;
    Ok(prefs)
}

#[tauri::command]
pub fn app_set_theme(
    app: AppHandle,
    request: SetThemeRequest,
) -> Result<AppPreferences, String> {
    let mut prefs = load_preferences(&app)?;
    prefs.theme = request.theme;
    save_preferences(&app, &prefs)?;
    Ok(prefs)
}

#[tauri::command]
pub fn app_pick_axis_plan_cache_dir() -> Result<Option<String>, String> {
    Ok(rfd::FileDialog::new()
        .pick_folder()
        .map(|path| path.display().to_string()))
}

#[tauri::command]
pub fn app_set_axis_plan_cache_dir(
    app: AppHandle,
    request: SetAxisPlanCacheDirRequest,
) -> Result<AppPreferences, String> {
    let axis_plan_cache_dir = request.path.and_then(|path| {
        let trimmed = path.trim();
        (!trimmed.is_empty()).then(|| trimmed.to_owned())
    });
    let mut prefs = load_preferences(&app)?;
    prefs.axis_plan_cache_dir = axis_plan_cache_dir;
    save_preferences(&app, &prefs)?;
    Ok(prefs)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_language_is_zh_cn() {
        assert_eq!(AppPreferences::default().language, AppLanguage::ZhCn);
    }

    #[test]
    fn default_theme_is_system() {
        assert_eq!(AppPreferences::default().theme, AppTheme::System);
    }

    #[test]
    fn language_serde_uses_stable_codes() {
        let prefs = AppPreferences {
            language: AppLanguage::En,
            axis_plan_cache_dir: None,
            theme: AppTheme::Dark,
        };
        let json = serde_json::to_string(&prefs).unwrap();
        assert!(json.contains("\"en\""));
        assert!(json.contains("\"dark\""));
        let parsed: AppPreferences = serde_json::from_str(r#"{"language":"zh-CN"}"#).unwrap();
        assert_eq!(parsed.language, AppLanguage::ZhCn);
    }

    #[test]
    fn theme_defaults_to_system_when_absent() {
        // Preferences written before the theme field existed must still load.
        let parsed: AppPreferences = serde_json::from_str(r#"{"language":"en"}"#).unwrap();
        assert_eq!(parsed.theme, AppTheme::System);
    }
}
