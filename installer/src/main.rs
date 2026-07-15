// Interactive installer for the ScoreTracker VPX plugin.
//
// Ships in the release ZIP next to the scoretracker/ payload folder. Asks where
// VPinballX lives and where the tables folder is, copies the plugin into VPX's
// plugins directory, enables it in VPinballX.ini, and records the tables folder
// so the companion app finds the scores on first launch.

use std::fs;
use std::io::{self, BufRead, Write};
use std::path::{Path, PathBuf};

const COMPANION_ID: &str = "com.antigravity.scoretracker.companion";

fn main() {
    if let Err(message) = run() {
        eprintln!("error: {message}");
        pause_before_exit();
        std::process::exit(1);
    }
    pause_before_exit();
}

fn run() -> Result<(), String> {
    let payload = exe_dir()?.join("scoretracker");
    if !payload.join("plugin.cfg").is_file() {
        return Err(format!(
            "plugin payload not found next to the installer (expected {})",
            payload.display()
        ));
    }
    let maps = payload.join("maps");
    if !maps.join("index.json").is_file()
        || !maps.join("maps").is_dir()
        || !maps.join("platforms").is_dir()
    {
        return Err(format!(
            "bundled NVRAM maps not found in the plugin payload (expected {})",
            maps.display()
        ));
    }

    // ---- 1. locate VPX and its plugins folder -------------------------------
    let vpx = prompt_path(vpx_question(), &default_vpx())?;
    if vpx.as_os_str().is_empty() {
        return Err("no VPinballX location given".into());
    }
    let plugins_dir = resolve_plugins_dir(&vpx)
        .ok_or_else(|| format!("could not find a VPinballX install at: {}", vpx.display()))?;

    // ---- 2. tables folder (recorded for the companion app) ------------------
    let tables = prompt_path(
        "Path to your VPX tables folder (used by the companion app)",
        &default_tables(),
    )?;
    if !tables.as_os_str().is_empty() && !tables.is_dir() {
        println!("warning: {} does not exist; recording it anyway", tables.display());
    }

    // ---- 3. copy the plugin --------------------------------------------------
    let dest = plugins_dir.join("scoretracker");
    if dest.exists() {
        fs::remove_dir_all(&dest).map_err(|e| format!("could not replace {}: {e}", dest.display()))?;
    }
    copy_dir(&payload, &dest)?;
    println!("Installed plugin to: {}", dest.display());

    // ---- 4. enable in VPinballX.ini ------------------------------------------
    match find_ini(&vpx) {
        Some(ini) => {
            enable_in_ini(&ini)?;
            println!("Enabled plugin in: {}", ini.display());
        }
        None => {
            println!("note: no VPinballX.ini found yet.");
            println!("      After launching VPX once, add to it:  [Plugin.ScoreTracker]  Enable = 1");
        }
    }

    // ---- 5. companion seed config --------------------------------------------
    let cfg_dir = companion_config_dir()?;
    fs::create_dir_all(&cfg_dir).map_err(|e| format!("could not create {}: {e}", cfg_dir.display()))?;
    let seed = cfg_dir.join("seed.json");
    let json = format!(
        "{{\n  \"tablesRoot\": \"{}\",\n  \"mapsRoot\": \"{}\"\n}}\n",
        json_escape(&tables.display().to_string()),
        json_escape(&dest.join("maps").display().to_string())
    );
    fs::write(&seed, json).map_err(|e| format!("could not write {}: {e}", seed.display()))?;
    println!("Companion seed written to: {}", seed.display());

    println!();
    println!("Done. Start a PinMAME table in VPX and finished games will appear in scores.json");
    println!("next to each table (and in the companion app).");
    Ok(())
}

// ---------------------------------------------------------------------------
// prompts and platform paths

fn vpx_question() -> &'static str {
    if cfg!(target_os = "macos") {
        "Path to your VPinballX app (the .app bundle) or the folder containing it"
    } else if cfg!(windows) {
        "Path to your VPinballX folder (the one containing VPinballX64.exe)"
    } else {
        "Path to your VPinballX folder (the one containing the VPinballX executable)"
    }
}

fn default_vpx() -> PathBuf {
    if cfg!(target_os = "macos") {
        for candidate in [
            PathBuf::from("/Applications/VPinballX_BGFX.app"),
            PathBuf::from("/Applications/VPinballX.app"),
        ] {
            if candidate.is_dir() {
                return candidate;
            }
        }
    }
    PathBuf::new()
}

fn default_tables() -> PathBuf {
    if let Some(home) = home_dir() {
        for candidate in [home.join("tables"), home.join(".vpinball").join("tables")] {
            if candidate.is_dir() {
                return candidate;
            }
        }
    }
    PathBuf::new()
}

fn resolve_plugins_dir(vpx: &Path) -> Option<PathBuf> {
    if cfg!(target_os = "macos") {
        if vpx.join("Contents/Resources").is_dir() {
            return Some(vpx.join("Contents/Resources/plugins"));
        }
        // folder containing the .app
        let mut apps: Vec<PathBuf> = fs::read_dir(vpx)
            .ok()?
            .flatten()
            .map(|e| e.path())
            .filter(|p| {
                p.extension().is_some_and(|x| x.eq_ignore_ascii_case("app"))
                    && p.file_name()
                        .and_then(|n| n.to_str())
                        .is_some_and(|n| n.to_ascii_lowercase().starts_with("vpinballx"))
            })
            .collect();
        apps.sort();
        return apps.first().map(|app| app.join("Contents/Resources/plugins"));
    }
    vpx.is_dir().then(|| vpx.join("plugins"))
}

/// Preference inis live in <SDL pref path>/VPinballX/<major.minor>/VPinballX.ini;
/// pick the newest version folder. Windows also supports a legacy ini beside the exe.
fn find_ini(vpx: &Path) -> Option<PathBuf> {
    let base = pref_base()?;
    let mut versions: Vec<(u32, u32, PathBuf)> = fs::read_dir(&base)
        .ok()?
        .flatten()
        .filter_map(|entry| {
            let ini = entry.path().join("VPinballX.ini");
            let name = entry.file_name().to_str()?.to_owned();
            let (major, minor) = name.split_once('.')?;
            Some((major.parse().ok()?, minor.parse().ok()?, ini))
        })
        .filter(|(_, _, ini)| ini.is_file())
        .collect();
    versions.sort();
    if let Some((_, _, ini)) = versions.last() {
        return Some(ini.clone());
    }
    let legacy = vpx.join("VPinballX.ini");
    legacy.is_file().then_some(legacy)
}

fn pref_base() -> Option<PathBuf> {
    if cfg!(target_os = "macos") {
        Some(home_dir()?.join("Library/Application Support/VPinballX"))
    } else if cfg!(windows) {
        Some(PathBuf::from(std::env::var_os("APPDATA")?).join("VPinballX"))
    } else {
        let data = std::env::var_os("XDG_DATA_HOME")
            .map(PathBuf::from)
            .unwrap_or(home_dir()?.join(".local/share"));
        Some(data.join("VPinballX"))
    }
}

fn companion_config_dir() -> Result<PathBuf, String> {
    let dir = if cfg!(target_os = "macos") {
        home_dir()
            .ok_or("cannot determine home directory")?
            .join("Library/Application Support")
            .join(COMPANION_ID)
    } else if cfg!(windows) {
        PathBuf::from(std::env::var_os("APPDATA").ok_or("APPDATA is not set")?).join(COMPANION_ID)
    } else {
        std::env::var_os("XDG_CONFIG_HOME")
            .map(PathBuf::from)
            .unwrap_or(
                home_dir()
                    .ok_or("cannot determine home directory")?
                    .join(".config"),
            )
            .join(COMPANION_ID)
    };
    Ok(dir)
}

fn home_dir() -> Option<PathBuf> {
    std::env::var_os(if cfg!(windows) { "USERPROFILE" } else { "HOME" }).map(PathBuf::from)
}

fn exe_dir() -> Result<PathBuf, String> {
    let exe = std::env::current_exe().map_err(|e| format!("cannot locate installer: {e}"))?;
    Ok(exe.parent().unwrap_or(Path::new(".")).to_path_buf())
}

fn prompt_path(question: &str, default: &Path) -> Result<PathBuf, String> {
    if default.as_os_str().is_empty() {
        print!("{question}: ");
    } else {
        print!("{question} [{}]: ", default.display());
    }
    io::stdout().flush().ok();
    let mut answer = String::new();
    io::stdin()
        .lock()
        .read_line(&mut answer)
        .map_err(|e| format!("could not read input: {e}"))?;
    let answer = answer.trim().trim_matches('"').to_owned();
    if answer.is_empty() {
        return Ok(default.to_path_buf());
    }
    if let Some(rest) = answer.strip_prefix("~/") {
        if let Some(home) = home_dir() {
            return Ok(home.join(rest));
        }
    }
    Ok(PathBuf::from(answer))
}

fn pause_before_exit() {
    // Keeps the console window readable when the installer is double-clicked.
    print!("\nPress Enter to close...");
    io::stdout().flush().ok();
    let mut sink = String::new();
    io::stdin().lock().read_line(&mut sink).ok();
}

// ---------------------------------------------------------------------------
// file operations

fn copy_dir(from: &Path, to: &Path) -> Result<(), String> {
    fs::create_dir_all(to).map_err(|e| format!("could not create {}: {e}", to.display()))?;
    for entry in fs::read_dir(from).map_err(|e| format!("could not read {}: {e}", from.display()))? {
        let entry = entry.map_err(|e| e.to_string())?;
        let target = to.join(entry.file_name());
        if entry.file_type().map_err(|e| e.to_string())?.is_dir() {
            copy_dir(&entry.path(), &target)?;
        } else {
            fs::copy(entry.path(), &target)
                .map_err(|e| format!("could not copy {}: {e}", entry.path().display()))?;
        }
    }
    Ok(())
}

/// Set Enable = 1 in the [Plugin.ScoreTracker] section, creating either as needed.
/// Everything else in the ini is preserved byte for byte.
fn enable_in_ini(ini: &Path) -> Result<(), String> {
    let text = fs::read_to_string(ini).map_err(|e| format!("could not read {}: {e}", ini.display()))?;
    let mut out: Vec<String> = Vec::with_capacity(text.lines().count() + 4);
    let mut in_section = false;
    let mut section_seen = false;
    let mut enabled = false;
    for line in text.lines() {
        let trimmed = line.trim_start();
        if trimmed.starts_with('[') {
            if in_section && !enabled {
                out.push("Enable = 1".into());
                enabled = true;
            }
            in_section = trimmed.starts_with("[Plugin.ScoreTracker]");
            if in_section {
                section_seen = true;
            }
            out.push(line.into());
            continue;
        }
        if in_section && !enabled && trimmed.to_ascii_lowercase().starts_with("enable") {
            let rest = trimmed["enable".len()..].trim_start();
            if rest.starts_with('=') {
                out.push("Enable = 1".into());
                enabled = true;
                continue;
            }
        }
        out.push(line.into());
    }
    if section_seen && !enabled {
        out.push("Enable = 1".into());
    }
    if !section_seen {
        if !out.last().is_none_or(|l| l.is_empty()) {
            out.push(String::new());
        }
        out.push("[Plugin.ScoreTracker]".into());
        out.push("Enable = 1".into());
    }
    let mut result = out.join("\n");
    result.push('\n');
    fs::write(ini, result).map_err(|e| format!("could not write {}: {e}", ini.display()))
}

fn json_escape(value: &str) -> String {
    value.replace('\\', "\\\\").replace('"', "\\\"")
}
