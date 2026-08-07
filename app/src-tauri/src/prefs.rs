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

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct AppPreferences {
    pub language: AppLanguage,
}

impl Default for AppPreferences {
    fn default() -> Self {
        Self {
            language: AppLanguage::ZhCn,
        }
    }
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SetLanguageRequest {
    pub language: AppLanguage,
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
    let prefs = AppPreferences {
        language: request.language,
    };
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
    fn language_serde_uses_stable_codes() {
        let prefs = AppPreferences {
            language: AppLanguage::En,
        };
        let json = serde_json::to_string(&prefs).unwrap();
        assert!(json.contains("\"en\""));
        let parsed: AppPreferences = serde_json::from_str(r#"{"language":"zh-CN"}"#).unwrap();
        assert_eq!(parsed.language, AppLanguage::ZhCn);
    }
}
