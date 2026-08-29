use serde::Deserialize;

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ExportArtifactRequest {
    pub default_name: String,
    pub extension: String,
    pub content: String,
}

/// Save a structured export artifact (JSON/CSV/TXT) through a save dialog.
/// Content is always generated from stored Project data by the frontend;
/// this command only handles the file dialog and the atomic write.
#[tauri::command]
pub fn export_artifact(request: ExportArtifactRequest) -> Result<String, String> {
    let extension = request
        .extension
        .trim_start_matches('.')
        .chars()
        .take(8)
        .collect::<String>();
    if extension.is_empty() || !extension.chars().all(|c| c.is_ascii_alphanumeric()) {
        return Err("invalid export extension".to_owned());
    }
    let file_name = if request.default_name.ends_with(&format!(".{extension}")) {
        request.default_name.clone()
    } else {
        format!("{}.{extension}", request.default_name)
    };
    let path = rfd::FileDialog::new()
        .set_file_name(&file_name)
        .add_filter(extension.to_uppercase(), &[extension.as_str()])
        .save_file()
        .ok_or_else(|| "cancelled".to_owned())?;
    crate::atomic_file::write_bytes(&path, request.content.as_bytes())
        .map_err(|error| format!("failed to write export: {error}"))?;
    Ok(path.display().to_string())
}
