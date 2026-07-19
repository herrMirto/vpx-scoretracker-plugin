mod nvram;
mod scores;
mod update;

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

/// Folder defaults written by the plugin installer (seed.json in the app config dir).
/// Consumed once on first run, when the user has not picked folders yet.
#[derive(serde::Serialize)]
pub struct SeedConfig {
    #[serde(rename = "tablesRoot")]
    tables_root: Option<String>,
    #[serde(rename = "mapsRoot")]
    maps_root: Option<String>,
}

#[tauri::command]
fn read_seed_config(app: tauri::AppHandle) -> Option<SeedConfig> {
    use tauri::Manager;
    let path = app.path().app_config_dir().ok()?.join("seed.json");
    let raw = std::fs::read_to_string(path).ok()?;
    let value: Value = serde_json::from_str(&raw).ok()?;
    let get = |key: &str| {
        value
            .get(key)
            .and_then(Value::as_str)
            .filter(|s| !s.is_empty())
            .map(str::to_owned)
    };
    Some(SeedConfig {
        tables_root: get("tablesRoot"),
        maps_root: get("mapsRoot"),
    })
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![
            scan_scores,
            resolve_vpx_hash,
            resolve_maps_root,
            read_seed_config,
            load_nvram,
            update::check_for_update,
            update::download_and_launch_update
        ])
        .run(tauri::generate_context!())
        .expect("error while running VPX Scoretracker Viewer");
}
