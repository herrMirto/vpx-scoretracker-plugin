// Installer for the ScoreTracker VPX plugin.
//
// Ships in the release ZIP with the scoretracker/ payload. Uses native dialogs
// to locate VPinballX and the tables folder, copies the plugin into VPX's plugins
// directory, enables it in VPinballX.ini, and prepares the companion app.

#![cfg_attr(target_os = "windows", windows_subsystem = "windows")]

use std::fs;
use std::path::{Path, PathBuf};

const COMPANION_ID: &str = "com.antigravity.scoretracker.companion";

fn main() {
    use rfd::{MessageButtons, MessageDialog, MessageLevel};

    match run_gui() {
        Ok(Some(result)) => {
            MessageDialog::new()
                .set_level(MessageLevel::Info)
                .set_title("ScoreTracker installed")
                .set_description(format!(
                    "ScoreTracker was installed successfully.\n\nPlugin: {}\nTables: {}\n\nStart a PinMAME table in VPX, then open the companion app.",
                    result.plugin_dir.display(),
                    result.tables_dir.display()
                ))
                .set_buttons(MessageButtons::Ok)
                .show();
        }
        Ok(None) => {}
        Err(message) => {
            MessageDialog::new()
                .set_level(MessageLevel::Error)
                .set_title("ScoreTracker could not be installed")
                .set_description(&message)
                .set_buttons(MessageButtons::Ok)
                .show();
        }
    }
}

#[derive(Debug)]
struct InstallResult {
    plugin_dir: PathBuf,
    tables_dir: PathBuf,
}

fn payload_dir() -> Result<PathBuf, String> {
    let exe = exe_dir()?;
    let candidates = [
        exe.join("scoretracker"),
        exe.parent()
            .unwrap_or(&exe)
            .join("Resources")
            .join("scoretracker"),
    ];
    candidates
        .into_iter()
        .find(|path| path.join("plugin.cfg").is_file())
        .ok_or_else(|| "plugin payload is missing from the installer".into())
}

fn validate_payload(payload: &Path) -> Result<(), String> {
    if !payload.join("plugin.cfg").is_file() {
        return Err(format!(
            "plugin payload not found (expected {})",
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

    Ok(())
}

fn install(payload: &Path, vpx: &Path, tables: &Path) -> Result<InstallResult, String> {
    let plugins_dir = resolve_plugins_dir(vpx).ok_or_else(|| {
        format!(
            "Could not find a VPinballX installation in {}",
            vpx.display()
        )
    })?;

    // ---- 3. copy the plugin --------------------------------------------------
    let dest = plugins_dir.join("scoretracker");
    if dest.exists() {
        fs::remove_dir_all(&dest)
            .map_err(|e| format!("could not replace {}: {e}", dest.display()))?;
    }
    copy_dir(&payload, &dest)?;

    // ---- 4. enable in VPinballX.ini ------------------------------------------
    match find_ini(&vpx) {
        Some(ini) => {
            enable_in_ini(&ini)?;
        }
        None => {}
    }

    // ---- 5. companion seed config --------------------------------------------
    let cfg_dir = companion_config_dir()?;
    fs::create_dir_all(&cfg_dir)
        .map_err(|e| format!("could not create {}: {e}", cfg_dir.display()))?;
    let seed = cfg_dir.join("seed.json");
    let json = format!(
        "{{\n  \"tablesRoot\": \"{}\",\n  \"mapsRoot\": \"{}\"\n}}\n",
        json_escape(&tables.display().to_string()),
        json_escape(&dest.join("maps").display().to_string())
    );
    fs::write(&seed, json).map_err(|e| format!("could not write {}: {e}", seed.display()))?;
    Ok(InstallResult {
        plugin_dir: dest,
        tables_dir: tables.to_path_buf(),
    })
}

fn run_gui() -> Result<Option<InstallResult>, String> {
    use rfd::{FileDialog, MessageButtons, MessageDialog, MessageDialogResult, MessageLevel};

    let payload = payload_dir()?;
    validate_payload(&payload)?;

    let mut selected_vpx = None;
    if let Some(detected) = detect_vpx() {
        let use_detected = MessageDialog::new()
            .set_level(MessageLevel::Info)
            .set_title("Install ScoreTracker")
            .set_description(format!(
                "VPinballX was found here:\n\n{}\n\nInstall ScoreTracker in this location?",
                detected.display()
            ))
            .set_buttons(MessageButtons::YesNo)
            .show();
        if use_detected == MessageDialogResult::Yes {
            selected_vpx = Some(detected);
        }
    }

    let vpx = match selected_vpx {
        Some(path) => path,
        None => loop {
            let mut picker = FileDialog::new().set_title("Choose the folder containing VPinballX");
            if let Some(home) = home_dir() {
                picker = picker.set_directory(home);
            }
            let Some(path) = picker.pick_folder() else {
                return Ok(None);
            };
            if resolve_plugins_dir(&path).is_some() {
                break path;
            }
            MessageDialog::new()
                .set_level(MessageLevel::Error)
                .set_title("VPinballX was not found")
                .set_description(vpx_picker_help())
                .set_buttons(MessageButtons::Ok)
                .show();
        },
    };

    let mut tables_picker = FileDialog::new().set_title("Choose your VPX Tables folder");
    let default_tables = default_tables();
    if default_tables.is_dir() {
        tables_picker = tables_picker.set_directory(default_tables);
    }
    let Some(tables) = tables_picker.pick_folder() else {
        return Ok(None);
    };

    let plugins_dir = resolve_plugins_dir(&vpx).ok_or_else(|| {
        format!(
            "Could not find a VPinballX installation in {}",
            vpx.display()
        )
    })?;
    let confirmed = MessageDialog::new()
        .set_level(MessageLevel::Info)
        .set_title("Ready to install")
        .set_description(format!(
            "Plugin location:\n{}\n\nTables folder:\n{}",
            plugins_dir.join("scoretracker").display(),
            tables.display()
        ))
        .set_buttons(MessageButtons::OkCancel)
        .show();
    if confirmed != MessageDialogResult::Ok {
        return Ok(None);
    }

    install(&payload, &vpx, &tables).map(Some)
}

// ---------------------------------------------------------------------------
// platform paths

fn detect_vpx() -> Option<PathBuf> {
    let mut roots = Vec::new();

    #[cfg(target_os = "macos")]
    roots.push(PathBuf::from("/Applications"));

    #[cfg(target_os = "windows")]
    {
        for variable in ["ProgramFiles", "ProgramFiles(x86)"] {
            if let Some(folder) = std::env::var_os(variable) {
                let folder = PathBuf::from(folder);
                roots.extend([folder.join("VPinball"), folder.join("Visual Pinball")]);
            }
        }
        roots.push(PathBuf::from(r"C:\Visual Pinball"));
    }

    #[cfg(target_os = "linux")]
    roots.extend([
        PathBuf::from("/opt/vpinball"),
        PathBuf::from("/usr/local/share/vpinball"),
    ]);

    if let Some(home) = home_dir() {
        #[cfg(target_os = "macos")]
        roots.push(home.join("Applications"));

        roots.extend([home.join("vpinball"), home.join("VPinballX")]);

        #[cfg(target_os = "windows")]
        roots.extend([
            home.join("Visual Pinball"),
            home.join("Documents/Visual Pinball"),
        ]);

        #[cfg(target_os = "linux")]
        roots.push(home.join(".local/share/VPinballX"));
    }
    roots
        .into_iter()
        .find_map(|root| resolve_vpx_location(&root))
}

fn default_tables() -> PathBuf {
    if let Some(home) = home_dir() {
        #[cfg(target_os = "windows")]
        let candidates = vec![
            home.join("tables"),
            home.join(".vpinball/tables"),
            home.join("Visual Pinball/Tables"),
            home.join("Documents/Visual Pinball/Tables"),
        ];
        #[cfg(not(target_os = "windows"))]
        let candidates = vec![home.join("tables"), home.join(".vpinball/tables")];
        for candidate in candidates {
            if candidate.is_dir() {
                return candidate;
            }
        }
    }
    PathBuf::new()
}

fn resolve_plugins_dir(vpx: &Path) -> Option<PathBuf> {
    let location = resolve_vpx_location(vpx)?;
    #[cfg(target_os = "macos")]
    return Some(location.join("Contents/Resources/plugins"));
    #[cfg(not(target_os = "macos"))]
    return Some(location.join("plugins"));
}

#[cfg(target_os = "macos")]
fn resolve_vpx_location(path: &Path) -> Option<PathBuf> {
    find_vpx_app(path)
}

#[cfg(not(target_os = "macos"))]
fn resolve_vpx_location(path: &Path) -> Option<PathBuf> {
    find_vpx_install_dir(path)
}

#[cfg(target_os = "macos")]
fn find_vpx_app(path: &Path) -> Option<PathBuf> {
    if is_vpx_app(path) {
        return Some(path.to_path_buf());
    }

    // Accept a folder containing the app, as well as a local source tree
    // whose build/ folder contains the app.
    let mut apps = Vec::new();
    for folder in [path.to_path_buf(), path.join("build")] {
        apps.extend(
            fs::read_dir(folder)
                .into_iter()
                .flatten()
                .flatten()
                .map(|entry| entry.path())
                .filter(|candidate| is_vpx_app(candidate)),
        );
    }
    apps.sort();
    apps.into_iter().next()
}

#[cfg(target_os = "macos")]
fn is_vpx_app(path: &Path) -> bool {
    path.extension()
        .is_some_and(|ext| ext.eq_ignore_ascii_case("app"))
        && path
            .file_name()
            .and_then(|name| name.to_str())
            .is_some_and(|name| name.to_ascii_lowercase().starts_with("vpinballx"))
        && path.join("Contents/Resources").is_dir()
}

#[cfg(not(target_os = "macos"))]
fn find_vpx_install_dir(path: &Path) -> Option<PathBuf> {
    let mut candidates = vec![
        path.to_path_buf(),
        path.join("bin"),
        path.join("build"),
        path.join("build/bin"),
        path.join("build/Release"),
        path.join("build/Release/bin"),
    ];
    candidates.extend(
        fs::read_dir(path)
            .into_iter()
            .flatten()
            .flatten()
            .map(|entry| entry.path())
            .filter(|candidate| candidate.is_dir()),
    );
    candidates.sort();
    candidates.into_iter().find(|candidate| {
        fs::read_dir(candidate)
            .into_iter()
            .flatten()
            .flatten()
            .any(|entry| is_vpx_executable(&entry.path()))
    })
}

#[cfg(target_os = "windows")]
fn is_vpx_executable(path: &Path) -> bool {
    path.is_file()
        && path
            .file_name()
            .and_then(|name| name.to_str())
            .is_some_and(|name| {
                let name = name.to_ascii_lowercase();
                name.starts_with("vpinballx") && name.ends_with(".exe")
            })
}

#[cfg(target_os = "linux")]
fn is_vpx_executable(path: &Path) -> bool {
    use std::os::unix::fs::PermissionsExt;

    path.is_file()
        && path
            .file_name()
            .and_then(|name| name.to_str())
            .is_some_and(|name| name.to_ascii_lowercase().starts_with("vpinballx"))
        && path
            .metadata()
            .is_ok_and(|metadata| metadata.permissions().mode() & 0o111 != 0)
}

fn vpx_picker_help() -> &'static str {
    if cfg!(target_os = "macos") {
        "Choose the folder containing the VPinballX app, or a local VPX folder whose build directory contains the app."
    } else if cfg!(target_os = "windows") {
        "Choose the Visual Pinball folder containing VPinballX.exe or VPinballX64.exe. Local build folders are also supported."
    } else {
        "Choose the folder containing the VPinballX executable. Local build folders are also supported."
    }
}

/// Preference inis live in <SDL pref path>/VPinballX/<major.minor>/VPinballX.ini;
/// pick the newest version folder. Windows also supports a legacy ini beside the exe.
fn find_ini(vpx: &Path) -> Option<PathBuf> {
    let mut versions: Vec<(u32, u32, PathBuf)> = pref_base()
        .and_then(|base| fs::read_dir(base).ok())
        .into_iter()
        .flatten()
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
    let legacy = resolve_vpx_location(vpx)?.join("VPinballX.ini");
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

// ---------------------------------------------------------------------------
// file operations

fn copy_dir(from: &Path, to: &Path) -> Result<(), String> {
    fs::create_dir_all(to).map_err(|e| format!("could not create {}: {e}", to.display()))?;
    for entry in
        fs::read_dir(from).map_err(|e| format!("could not read {}: {e}", from.display()))?
    {
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
    let text =
        fs::read_to_string(ini).map_err(|e| format!("could not read {}: {e}", ini.display()))?;
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

#[cfg(all(test, target_os = "macos"))]
mod tests {
    use super::*;

    fn test_root(name: &str) -> PathBuf {
        let root = std::env::temp_dir().join(format!(
            "scoretracker-installer-{name}-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        root
    }

    fn create_vpx_app(path: &Path) {
        fs::create_dir_all(path.join("Contents/Resources")).unwrap();
    }

    #[test]
    fn accepts_the_app_bundle_itself() {
        let root = test_root("app");
        let app = root.join("VPinballX_BGFX.app");
        create_vpx_app(&app);
        assert_eq!(find_vpx_app(&app), Some(app.clone()));
        assert_eq!(
            resolve_plugins_dir(&app),
            Some(app.join("Contents/Resources/plugins"))
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn accepts_a_local_source_tree_with_an_app_in_build() {
        let root = test_root("source-tree");
        let app = root.join("build/VPinballX_BGFX.app");
        create_vpx_app(&app);
        assert_eq!(find_vpx_app(&root), Some(app.clone()));
        assert_eq!(
            resolve_plugins_dir(&root),
            Some(app.join("Contents/Resources/plugins"))
        );
        fs::remove_dir_all(root).unwrap();
    }
}

#[cfg(all(test, not(target_os = "macos")))]
mod non_macos_tests {
    use super::*;

    fn test_root(name: &str) -> PathBuf {
        let root = std::env::temp_dir().join(format!(
            "scoretracker-installer-{name}-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        root
    }

    fn create_vpx_executable(folder: &Path) {
        fs::create_dir_all(folder).unwrap();
        #[cfg(target_os = "windows")]
        let executable = folder.join("VPinballX64.exe");
        #[cfg(target_os = "linux")]
        let executable = folder.join("VPinballX_GL");
        fs::write(&executable, b"test executable").unwrap();
        #[cfg(target_os = "linux")]
        {
            use std::os::unix::fs::PermissionsExt;
            let mut permissions = executable.metadata().unwrap().permissions();
            permissions.set_mode(0o755);
            fs::set_permissions(&executable, permissions).unwrap();
        }
    }

    #[test]
    fn accepts_an_install_folder_containing_the_vpx_executable() {
        let root = test_root("install-folder");
        create_vpx_executable(&root);
        assert_eq!(find_vpx_install_dir(&root), Some(root.clone()));
        assert_eq!(resolve_plugins_dir(&root), Some(root.join("plugins")));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn accepts_a_local_source_tree_with_an_executable_in_build() {
        let root = test_root("source-tree");
        let build = root.join("build");
        create_vpx_executable(&build);
        assert_eq!(find_vpx_install_dir(&root), Some(build.clone()));
        assert_eq!(resolve_plugins_dir(&root), Some(build.join("plugins")));
        fs::remove_dir_all(root).unwrap();
    }
}
