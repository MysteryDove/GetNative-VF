mod atomic_file;
mod engine;
mod export;
mod media;
mod prefs;
mod project;
mod worker;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .manage(worker::WorkerManager::default())
        .invoke_handler(tauri::generate_handler![
            engine::engine_capabilities,
            engine::engine_geometry,
            export::export_artifact,
            worker::engine_worker_start,
            worker::engine_worker_capabilities,
            worker::engine_worker_media_begin,
            worker::engine_worker_media_read_asset,
            worker::engine_worker_analyze,
            worker::engine_worker_cancel,
            worker::engine_worker_shutdown,
            worker::engine_worker_verify_begin,
            worker::engine_worker_verify_media_begin,
            worker::engine_worker_verify_frame,
            worker::engine_worker_verify_end,
            media::media_frame_asset,
            media::media_pick_files,
            media::media_probe,
            media::media_preview,
            project::project_create,
            project::project_create_untitled,
            project::project_open,
            project::project_save,
            project::project_autosave,
            project::project_list_recent,
            project::project_remove_recent,
            project::project_reveal,
            project::project_recovery_info,
            project::project_recover,
            project::project_discard_recovery,
            prefs::app_get_preferences,
            prefs::app_set_language,
            prefs::app_pick_axis_plan_cache_dir,
            prefs::app_set_axis_plan_cache_dir,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
