mod nvram;
mod scores;

use std::collections::HashMap;

use serde_json::Value;

#[tauri::command]
async fn scan_scores(tables_root: String) -> Result<scores::ScanSnapshot, String> {
    tauri::async_runtime::spawn_blocking(move || scores::scan(&tables_root))
        .await
        .map_err(|error| format!("score scan task failed: {error}"))?
        .map_err(|error| error.to_string())
}

#[tauri::command]
async fn resolve_vpx_hash(
    tables_root: String,
    score_source: String,
) -> Result<Option<String>, String> {
    tauri::async_runtime::spawn_blocking(move || {
        scores::hash_vpx_for_source(&tables_root, &score_source)
    })
    .await
    .map_err(|error| format!("VPX hash task failed: {error}"))?
}

#[tauri::command]
fn load_nvram(
    tables_root: String,
    maps_root: String,
    rom: String,
    score_source: String,
) -> Result<Option<nvram::NvramDocument>, String> {
    nvram::load(&tables_root, &maps_root, &rom, &score_source)
}

#[tauri::command]
fn resolve_maps_root(path: String) -> Result<String, String> {
    nvram::resolve_maps_root(&path)
}

#[tauri::command]
fn save_nvram(
    tables_root: String,
    maps_root: String,
    rom: String,
    nvram_path: String,
    changes: HashMap<String, Value>,
) -> Result<nvram::SaveResult, String> {
    nvram::save(&tables_root, &maps_root, &rom, &nvram_path, changes)
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![
            scan_scores,
            resolve_vpx_hash,
            resolve_maps_root,
            load_nvram,
            save_nvram
        ])
        .run(tauri::generate_context!())
        .expect("error while running ScoreTracker Companion");
}
