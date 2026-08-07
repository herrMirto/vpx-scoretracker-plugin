mod nvram;
mod scores;
mod update;

use serde_json::Value;
use std::{
    fs::{self, OpenOptions},
    io::Write,
    path::PathBuf,
    process::Command,
    time::Duration,
};
use tauri::Manager;

const DIAGNOSTIC_LOG_NAME: &str = "scoretracker-viewer.log";
const DIAGNOSTIC_LOG_LIMIT: u64 = 2 * 1024 * 1024;
const LEGACY_COMPANION_ID: &str = "com.antigravity.scoretracker.companion";
const ALLOWED_MEDIA_URLS: [&str; 2] = [
    "https://api.vpinplay.com:8888/",
    "https://raw.githubusercontent.com/superhac/vpinmediadb/",
];

fn diagnostic_log_file(app: &tauri::AppHandle) -> Result<PathBuf, String> {
    app.path()
        .app_log_dir()
        .map(|directory| directory.join(DIAGNOSTIC_LOG_NAME))
        .map_err(|error| format!("could not resolve application log directory: {error}"))
}

fn single_line(value: &str) -> String {
    value.replace(['\r', '\n'], " ")
}

#[tauri::command]
fn diagnostic_log_path(app: tauri::AppHandle) -> Result<String, String> {
    diagnostic_log_file(&app).map(|path| path.to_string_lossy().into_owned())
}

#[tauri::command]
fn reveal_diagnostic_log(app: tauri::AppHandle) -> Result<(), String> {
    let path = diagnostic_log_file(&app)?;
    let directory = path
        .parent()
        .ok_or_else(|| "diagnostic log path has no parent directory".to_owned())?;
    fs::create_dir_all(directory)
        .map_err(|error| format!("could not create diagnostic log directory: {error}"))?;
    OpenOptions::new()
        .create(true)
        .append(true)
        .open(&path)
        .map_err(|error| format!("could not create diagnostic log: {error}"))?;

    #[cfg(target_os = "macos")]
    let mut command = {
        let mut command = Command::new("open");
        command.arg("-R").arg(&path);
        command
    };
    #[cfg(target_os = "windows")]
    let mut command = {
        let mut command = Command::new("explorer.exe");
        command.arg(format!("/select,{}", path.display()));
        command
    };
    #[cfg(not(any(target_os = "macos", target_os = "windows")))]
    let mut command = {
        let mut command = Command::new("xdg-open");
        command.arg(directory);
        command
    };

    command
        .spawn()
        .map(|_| ())
        .map_err(|error| format!("could not reveal diagnostic log: {error}"))
}

#[tauri::command]
fn write_diagnostic_log(
    app: tauri::AppHandle,
    level: String,
    event: String,
    details: Value,
) -> Result<(), String> {
    let path = diagnostic_log_file(&app)?;
    let directory = path
        .parent()
        .ok_or_else(|| "diagnostic log path has no parent directory".to_owned())?;
    fs::create_dir_all(directory)
        .map_err(|error| format!("could not create diagnostic log directory: {error}"))?;

    if path.metadata().map(|metadata| metadata.len()).unwrap_or(0) >= DIAGNOSTIC_LOG_LIMIT {
        let previous = path.with_extension("previous.log");
        let _ = fs::remove_file(&previous);
        fs::rename(&path, previous)
            .map_err(|error| format!("could not rotate diagnostic log: {error}"))?;
    }

    let timestamp = chrono::Utc::now().to_rfc3339_opts(chrono::SecondsFormat::Millis, true);
    let encoded = serde_json::to_string(&details)
        .map_err(|error| format!("could not encode diagnostic details: {error}"))?;
    let mut file = OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)
        .map_err(|error| format!("could not open diagnostic log: {error}"))?;
    writeln!(
        file,
        "{} [{}] {} version={} {}",
        timestamp,
        single_line(&level).to_uppercase(),
        single_line(&event),
        env!("CARGO_PKG_VERSION"),
        encoded
    )
    .map_err(|error| format!("could not write diagnostic log: {error}"))
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
struct RemoteProbe {
    status: u16,
    content_type: Option<String>,
    content_length: Option<u64>,
}

#[tauri::command]
async fn probe_remote_url(url: String) -> Result<RemoteProbe, String> {
    if !ALLOWED_MEDIA_URLS
        .iter()
        .any(|prefix| url.starts_with(prefix))
    {
        return Err("remote probe URL is not an allowed ScoreTracker media endpoint".to_owned());
    }
    let client = reqwest::Client::builder()
        .timeout(Duration::from_secs(12))
        .build()
        .map_err(|error| format!("could not create diagnostic HTTP client: {error}"))?;
    let response = client
        .get(&url)
        .send()
        .await
        .map_err(|error| format!("remote probe failed: {error:#}"))?;
    Ok(RemoteProbe {
        status: response.status().as_u16(),
        content_type: response
            .headers()
            .get(reqwest::header::CONTENT_TYPE)
            .and_then(|value| value.to_str().ok())
            .map(str::to_owned),
        content_length: response.content_length(),
    })
}

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
async fn remove_game(
    tables_root: String,
    score_source: String,
    source_index: usize,
    score_id: Option<i64>,
) -> Result<(), String> {
    tauri::async_runtime::spawn_blocking(move || {
        scores::remove_game(&tables_root, &score_source, source_index, score_id)
    })
    .await
    .map_err(|error| format!("remove game task failed: {error}"))?
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
    let config_dir = app.path().app_config_dir().ok()?;
    let current_path = config_dir.join("seed.json");
    let legacy_path = config_dir
        .parent()
        .map(|parent| parent.join(LEGACY_COMPANION_ID).join("seed.json"));
    let (raw, migrated) = match std::fs::read_to_string(&current_path) {
        Ok(raw) => (raw, false),
        Err(_) => (std::fs::read_to_string(legacy_path?).ok()?, true),
    };
    if migrated && std::fs::create_dir_all(&config_dir).is_ok() {
        let _ = std::fs::write(&current_path, &raw);
    }
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
            remove_game,
            resolve_vpx_hash,
            resolve_maps_root,
            read_seed_config,
            load_nvram,
            diagnostic_log_path,
            reveal_diagnostic_log,
            write_diagnostic_log,
            probe_remote_url,
            update::check_for_update,
            update::download_and_launch_update
        ])
        .run(tauri::generate_context!())
        .expect("error while running VPX Scoretracker Viewer");
}
