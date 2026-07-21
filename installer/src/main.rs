// Installer for the ScoreTracker VPX plugin.
//
// Embeds the platform-specific plugin and Viewer payload in one executable.
// Shows a window with folder pickers for VPinballX and the tables folder,
// installs the bundled files, enables the plugin in VPinballX.ini, and seeds
// the Viewer configuration.

#![cfg_attr(target_os = "windows", windows_subsystem = "windows")]

use std::fs;
use std::io::Cursor;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::mpsc::{self, Receiver};
use std::thread;
use std::time::Duration;

use eframe::egui;
use flate2::read::GzDecoder;
use serde::{Deserialize, Serialize};

const COMPANION_ID: &str = "com.antigravity.scoretracker.companion";
const COMPANION_APP_NAME: &str = "VPX Scoretracker Viewer";
const LEGACY_COMPANION_APP_NAME: &str = "VPX Scoretracker Visualiser";
const EMBEDDED_PAYLOAD: &[u8] =
    include_bytes!(concat!(env!("OUT_DIR"), "/scoretracker-payload.tar.gz"));

#[derive(Debug, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct InstallConfig {
    vpx_root: Option<String>,
    tables_root: Option<String>,
    maps_root: Option<String>,
    version: Option<String>,
    create_desktop_icon: Option<bool>,
}

#[derive(Debug, Default, PartialEq, Eq)]
struct LaunchOptions {
    automatic_update: bool,
    mounted_volume: Option<PathBuf>,
}

fn launch_options(args: impl IntoIterator<Item = String>) -> LaunchOptions {
    let mut options = LaunchOptions::default();
    let mut args = args.into_iter().skip(1);
    while let Some(argument) = args.next() {
        match argument.as_str() {
            "--automatic-update" => options.automatic_update = true,
            "--mounted-volume" => options.mounted_volume = args.next().map(PathBuf::from),
            _ => {}
        }
    }
    options
}

fn desktop_icon_preference(saved: Option<bool>, automatic_update: bool) -> bool {
    saved.unwrap_or(!automatic_update)
}

fn main() -> Result<(), eframe::Error> {
    let launch = launch_options(std::env::args());
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([720.0, 560.0])
            .with_min_inner_size([640.0, 500.0]),
        centered: true,
        ..Default::default()
    };
    eframe::run_native(
        "ScoreTracker Installer",
        options,
        Box::new(move |cc| Ok(Box::new(InstallerApp::new(cc, launch)))),
    )
}

#[derive(Debug)]
struct InstallResult {
    plugin_dir: PathBuf,
    tables_dir: PathBuf,
    companion_status: String,
    companion_path: Option<PathBuf>,
}

fn extract_payload() -> Result<tempfile::TempDir, String> {
    extract_payload_from(EMBEDDED_PAYLOAD)
}

fn extract_payload_from(archive_bytes: &[u8]) -> Result<tempfile::TempDir, String> {
    let temp =
        tempfile::tempdir().map_err(|e| format!("could not create temporary folder: {e}"))?;
    let decoder = GzDecoder::new(Cursor::new(archive_bytes));
    let mut archive = tar::Archive::new(decoder);
    archive
        .unpack(temp.path())
        .map_err(|e| format!("could not extract the bundled installer files: {e}"))?;
    Ok(temp)
}

#[cfg(test)]
mod payload_tests {
    use super::*;
    use flate2::write::GzEncoder;
    use flate2::Compression;

    #[test]
    fn extracts_and_validates_an_embedded_payload() {
        let source = tempfile::tempdir().unwrap();
        let plugin = source.path().join("scoretracker");
        fs::create_dir_all(plugin.join("maps/maps")).unwrap();
        fs::create_dir_all(plugin.join("maps/platforms")).unwrap();
        fs::write(plugin.join("plugin.cfg"), b"[configuration]\n").unwrap();
        fs::write(plugin.join("maps/index.json"), b"{}\n").unwrap();

        let mut bytes = Vec::new();
        {
            let encoder = GzEncoder::new(&mut bytes, Compression::fast());
            let mut archive = tar::Builder::new(encoder);
            archive.append_dir_all(".", source.path()).unwrap();
            let encoder = archive.into_inner().unwrap();
            encoder.finish().unwrap();
        }

        let extracted = extract_payload_from(&bytes).unwrap();
        validate_payload(&extracted.path().join("scoretracker")).unwrap();
    }

    #[test]
    fn persists_update_paths_with_viewer_compatible_keys() {
        let config = InstallConfig {
            vpx_root: Some("/vpx".into()),
            tables_root: Some("/tables".into()),
            maps_root: Some("/maps".into()),
            version: Some("0.1.0".into()),
            create_desktop_icon: Some(true),
        };
        let value = serde_json::to_value(config).unwrap();
        assert_eq!(value["vpxRoot"], "/vpx");
        assert_eq!(value["tablesRoot"], "/tables");
        assert_eq!(value["mapsRoot"], "/maps");
        assert_eq!(value["version"], "0.1.0");
        assert_eq!(value["createDesktopIcon"], true);
    }

    #[test]
    fn parses_automatic_update_launch_options() {
        let options = launch_options(
            [
                "scoretracker-installer",
                "--automatic-update",
                "--mounted-volume",
                "/Volumes/ScoreTracker Installer",
            ]
            .into_iter()
            .map(str::to_owned),
        );
        assert_eq!(
            options,
            LaunchOptions {
                automatic_update: true,
                mounted_volume: Some(PathBuf::from("/Volumes/ScoreTracker Installer")),
            }
        );
    }

    #[test]
    fn manual_install_is_the_default_launch_mode() {
        let options = launch_options(["scoretracker-installer"].into_iter().map(str::to_owned));
        assert_eq!(options, LaunchOptions::default());
    }

    #[test]
    fn desktop_icon_is_checked_by_default_and_saved_choices_are_preserved() {
        assert!(desktop_icon_preference(None, false));
        assert!(!desktop_icon_preference(None, true));
        assert!(desktop_icon_preference(Some(true), true));
        assert!(!desktop_icon_preference(Some(false), false));
    }
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

fn install(
    vpx: &Path,
    tables: &Path,
    create_desktop_icon: bool,
    mut progress: impl FnMut(f32, &str),
) -> Result<InstallResult, String> {
    progress(0.05, "Extracting bundled files…");
    let extracted = extract_payload()?;
    let payload = extracted.path().join("scoretracker");
    validate_payload(&payload)?;

    progress(0.20, "Locating Visual Pinball X…");
    let plugins_dir = resolve_plugins_dir(vpx).ok_or_else(|| {
        format!(
            "Could not find a VPinballX installation in {}",
            vpx.display()
        )
    })?;

    progress(0.30, "Installing the ScoreTracker plugin and maps…");
    let dest = plugins_dir.join("scoretracker");
    if dest.exists() {
        fs::remove_dir_all(&dest)
            .map_err(|e| format!("could not replace {}: {e}", dest.display()))?;
    }
    copy_dir(&payload, &dest)?;

    progress(0.58, "Enabling the plugin…");
    if let Some(ini) = find_ini(vpx) {
        enable_in_ini(&ini)?;
    }

    progress(0.68, "Writing Viewer configuration…");
    let cfg_dir = companion_config_dir()?;
    fs::create_dir_all(&cfg_dir)
        .map_err(|e| format!("could not create {}: {e}", cfg_dir.display()))?;
    let seed = cfg_dir.join("seed.json");
    let config = InstallConfig {
        vpx_root: Some(vpx.display().to_string()),
        tables_root: Some(tables.display().to_string()),
        maps_root: Some(dest.join("maps").display().to_string()),
        version: Some(env!("CARGO_PKG_VERSION").to_owned()),
        create_desktop_icon: Some(create_desktop_icon),
    };
    let json = serde_json::to_string_pretty(&config)
        .map_err(|e| format!("could not serialize Viewer configuration: {e}"))?;
    fs::write(&seed, format!("{json}\n"))
        .map_err(|e| format!("could not write {}: {e}", seed.display()))?;

    progress(0.78, "Installing VPX Scoretracker Viewer…");
    // A failed Viewer copy must not roll back the plugin install that already
    // succeeded, but the completion screen makes the failure explicit.
    let (companion_status, companion_path) = match companion_payload(extracted.path()) {
        None => (
            format!("{COMPANION_APP_NAME} is not bundled with this installer."),
            None,
        ),
        Some(bundle) => {
            match companion_dest_root().and_then(|root| install_companion(&bundle, &root)) {
                Ok(installed) => {
                    let status = match sync_desktop_shortcut(&installed, create_desktop_icon) {
                        Ok(()) => format!("Viewer: {}", installed.display()),
                        Err(error) => format!(
                            "Viewer: {}\nDesktop icon could not be updated: {error}",
                            installed.display()
                        ),
                    };
                    (status, Some(installed))
                }
                Err(e) => (
                    format!("{COMPANION_APP_NAME} could not be installed: {e}"),
                    None,
                ),
            }
        }
    };

    progress(1.0, "Installation complete");
    Ok(InstallResult {
        plugin_dir: dest,
        tables_dir: tables.to_path_buf(),
        companion_status,
        companion_path,
    })
}

// ---------------------------------------------------------------------------
// companion app (VPX Scoretracker Viewer)

/// CI embeds the Viewer with the installer; locate it in the extracted payload.
fn companion_payload(payload_root: &Path) -> Option<PathBuf> {
    #[cfg(target_os = "macos")]
    {
        let path = payload_root.join(format!("{COMPANION_APP_NAME}.app"));
        path.is_dir().then_some(path)
    }
    #[cfg(target_os = "windows")]
    {
        let path = payload_root.join("vpx-scoretracker-viewer.exe");
        path.is_file().then_some(path)
    }
    #[cfg(target_os = "linux")]
    {
        let path = payload_root.join("vpx-scoretracker-viewer");
        path.is_file().then_some(path)
    }
}

/// Per-user, no-elevation location the user can reach through the OS app launcher.
fn companion_dest_root() -> Result<PathBuf, String> {
    #[cfg(target_os = "macos")]
    return Ok(home_dir()
        .ok_or("cannot determine home directory")?
        .join("Applications"));
    #[cfg(target_os = "windows")]
    return Ok(
        PathBuf::from(std::env::var_os("LOCALAPPDATA").ok_or("LOCALAPPDATA is not set")?)
            .join("Programs"),
    );
    #[cfg(target_os = "linux")]
    return Ok(home_dir()
        .ok_or("cannot determine home directory")?
        .join(".local/bin"));
}

#[cfg(target_os = "macos")]
fn install_companion(bundle: &Path, apps_dir: &Path) -> Result<PathBuf, String> {
    fs::create_dir_all(apps_dir)
        .map_err(|e| format!("could not create {}: {e}", apps_dir.display()))?;
    let dest = apps_dir.join(format!("{COMPANION_APP_NAME}.app"));
    if dest.exists() {
        fs::remove_dir_all(&dest)
            .map_err(|e| format!("could not replace {}: {e}", dest.display()))?;
    }
    // ditto preserves permissions, extended attributes and code signatures
    let status = std::process::Command::new("ditto")
        .arg(bundle)
        .arg(&dest)
        .status()
        .map_err(|e| format!("could not run ditto: {e}"))?;
    if !status.success() {
        return Err(format!("ditto failed copying to {}", dest.display()));
    }
    let legacy = apps_dir.join(format!("{LEGACY_COMPANION_APP_NAME}.app"));
    let _ = fs::remove_dir_all(legacy);
    Ok(dest)
}

#[cfg(target_os = "windows")]
fn install_companion(exe: &Path, programs_dir: &Path) -> Result<PathBuf, String> {
    let dir = programs_dir.join(COMPANION_APP_NAME);
    fs::create_dir_all(&dir).map_err(|e| format!("could not create {}: {e}", dir.display()))?;
    let dest = dir.join(format!("{COMPANION_APP_NAME}.exe"));
    copy_file_with_retry(exe, &dest)?;
    let _ = fs::remove_dir_all(programs_dir.join(LEGACY_COMPANION_APP_NAME));
    // Start Menu shortcut, best effort: the app is reachable in Programs either way
    if let Some(appdata) = std::env::var_os("APPDATA") {
        let shortcuts = PathBuf::from(appdata).join("Microsoft\\Windows\\Start Menu\\Programs");
        let _ = fs::remove_file(shortcuts.join(format!("{LEGACY_COMPANION_APP_NAME}.lnk")));
        let lnk = shortcuts.join(format!("{COMPANION_APP_NAME}.lnk"));
        let script = format!(
            "$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{}');$s.TargetPath='{}';$s.Save()",
            lnk.display(),
            dest.display()
        );
        let _ = std::process::Command::new("powershell")
            .args([
                "-NoProfile",
                "-NonInteractive",
                "-WindowStyle",
                "Hidden",
                "-Command",
                &script,
            ])
            .status();
    }
    Ok(dest)
}

#[cfg(target_os = "linux")]
fn install_companion(binary: &Path, bin_dir: &Path) -> Result<PathBuf, String> {
    use std::os::unix::fs::PermissionsExt;

    fs::create_dir_all(bin_dir)
        .map_err(|e| format!("could not create {}: {e}", bin_dir.display()))?;
    let dest = bin_dir.join("vpx-scoretracker-viewer");
    copy_file_with_retry(binary, &dest)?;
    fs::set_permissions(&dest, fs::Permissions::from_mode(0o755))
        .map_err(|e| format!("could not mark {} executable: {e}", dest.display()))?;
    let _ = fs::remove_file(bin_dir.join("vpx-scoretracker-viewer.AppImage"));
    let _ = fs::remove_file(bin_dir.join("vpx-scoretracker-visualiser.AppImage"));
    let icon_path = install_linux_icon(binary).ok();
    // Desktop entry, best effort: the binary runs from ~/.local/bin either way
    if let Some(home) = home_dir() {
        let apps = home.join(".local/share/applications");
        if fs::create_dir_all(&apps).is_ok() {
            let _ = fs::remove_file(apps.join("vpx-scoretracker-visualiser.desktop"));
            let entry = linux_desktop_entry(&dest, icon_path.as_deref());
            let _ = fs::write(apps.join("vpx-scoretracker-viewer.desktop"), entry);
        }
    }
    Ok(dest)
}

#[cfg(target_os = "linux")]
fn install_linux_icon(payload_binary: &Path) -> Result<PathBuf, String> {
    let source = payload_binary.with_file_name("vpx-scoretracker-viewer.png");
    if !source.is_file() {
        return Err("Viewer icon is not bundled".into());
    }
    let data_home = std::env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .or_else(|| home_dir().map(|home| home.join(".local/share")))
        .ok_or("cannot determine the user data directory")?;
    let icons = data_home.join("icons/hicolor/512x512/apps");
    fs::create_dir_all(&icons)
        .map_err(|error| format!("could not create {}: {error}", icons.display()))?;
    let dest = icons.join("vpx-scoretracker-viewer.png");
    fs::copy(&source, &dest)
        .map_err(|error| format!("could not install {}: {error}", dest.display()))?;
    Ok(dest)
}

#[cfg(target_os = "linux")]
fn linux_desktop_entry(executable: &Path, icon: Option<&Path>) -> String {
    let escape = |path: &Path| {
        path.display()
            .to_string()
            .replace('\\', "\\\\")
            .replace('"', "\\\"")
            .replace('`', "\\`")
            .replace('$', "\\$")
    };
    let icon_line = icon
        .map(|path| format!("Icon={}\n", path.display()))
        .unwrap_or_default();
    format!(
        "[Desktop Entry]\nType=Application\nName={COMPANION_APP_NAME}\nExec=\"{}\"\n{icon_line}Categories=Game;\nTerminal=false\n",
        escape(executable)
    )
}

#[cfg(target_os = "macos")]
fn sync_desktop_shortcut(app: &Path, create: bool) -> Result<(), String> {
    let desktop = home_dir()
        .ok_or("cannot determine home directory")?
        .join("Desktop");
    sync_macos_desktop_shortcut(app, &desktop, create)
}

#[cfg(target_os = "macos")]
fn sync_macos_desktop_shortcut(app: &Path, desktop: &Path, create: bool) -> Result<(), String> {
    use std::os::unix::fs::symlink;

    fs::create_dir_all(desktop)
        .map_err(|error| format!("could not create {}: {error}", desktop.display()))?;
    let shortcut = desktop.join(format!("{COMPANION_APP_NAME}.app"));
    remove_managed_symlink(&shortcut)?;
    if create {
        symlink(app, &shortcut)
            .map_err(|error| format!("could not create {}: {error}", shortcut.display()))?;
    }
    Ok(())
}

#[cfg(target_os = "macos")]
fn remove_managed_symlink(path: &Path) -> Result<(), String> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.file_type().is_symlink() => fs::remove_file(path)
            .map_err(|error| format!("could not replace {}: {error}", path.display())),
        Ok(_) => Err(format!(
            "{} already exists and was not created by ScoreTracker",
            path.display()
        )),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(format!("could not inspect {}: {error}", path.display())),
    }
}

#[cfg(target_os = "windows")]
fn sync_desktop_shortcut(app: &Path, create: bool) -> Result<(), String> {
    let script = r#"
$ErrorActionPreference = 'Stop'
$desktop = [Environment]::GetFolderPath('Desktop')
if ([string]::IsNullOrWhiteSpace($desktop)) { throw 'Windows Desktop folder was not found' }
$shortcut = Join-Path $desktop 'VPX Scoretracker Viewer.lnk'
if ($env:SCORETRACKER_CREATE_DESKTOP_ICON -eq '1') {
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut($shortcut)
    $link.TargetPath = $env:SCORETRACKER_SHORTCUT_TARGET
    $link.WorkingDirectory = Split-Path $env:SCORETRACKER_SHORTCUT_TARGET
    $link.Save()
} else {
    Remove-Item -LiteralPath $shortcut -Force -ErrorAction SilentlyContinue
}
"#;
    let status = Command::new("powershell")
        .args([
            "-NoProfile",
            "-NonInteractive",
            "-WindowStyle",
            "Hidden",
            "-Command",
            script,
        ])
        .env("SCORETRACKER_SHORTCUT_TARGET", app)
        .env(
            "SCORETRACKER_CREATE_DESKTOP_ICON",
            if create { "1" } else { "0" },
        )
        .status()
        .map_err(|error| format!("could not run PowerShell: {error}"))?;
    status
        .success()
        .then_some(())
        .ok_or_else(|| "PowerShell could not update the Desktop shortcut".into())
}

#[cfg(target_os = "linux")]
fn sync_desktop_shortcut(app: &Path, create: bool) -> Result<(), String> {
    use std::os::unix::fs::PermissionsExt;

    let desktop = linux_desktop_dir()?;
    fs::create_dir_all(&desktop)
        .map_err(|error| format!("could not create {}: {error}", desktop.display()))?;
    let shortcut = desktop.join("VPX Scoretracker Viewer.desktop");
    let _ = fs::remove_file(desktop.join("VPX Scoretracker Visualiser.desktop"));
    if !create {
        return match fs::remove_file(&shortcut) {
            Ok(()) => Ok(()),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(error) => Err(format!("could not remove {}: {error}", shortcut.display())),
        };
    }

    let icon = installed_linux_icon();
    fs::write(&shortcut, linux_desktop_entry(app, icon.as_deref()))
        .map_err(|error| format!("could not create {}: {error}", shortcut.display()))?;
    fs::set_permissions(&shortcut, fs::Permissions::from_mode(0o755))
        .map_err(|error| format!("could not mark {} executable: {error}", shortcut.display()))?;
    let _ = Command::new("gio")
        .args([
            "set",
            shortcut.to_string_lossy().as_ref(),
            "metadata::trusted",
            "true",
        ])
        .status();
    Ok(())
}

#[cfg(target_os = "linux")]
fn installed_linux_icon() -> Option<PathBuf> {
    let data_home = std::env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .or_else(|| home_dir().map(|home| home.join(".local/share")))?;
    let icon = data_home.join("icons/hicolor/512x512/apps/vpx-scoretracker-viewer.png");
    icon.is_file().then_some(icon)
}

#[cfg(target_os = "linux")]
fn linux_desktop_dir() -> Result<PathBuf, String> {
    if let Some(path) = std::env::var_os("XDG_DESKTOP_DIR") {
        return Ok(PathBuf::from(path));
    }
    if let Ok(output) = Command::new("xdg-user-dir").arg("DESKTOP").output() {
        if output.status.success() {
            let path = String::from_utf8_lossy(&output.stdout).trim().to_owned();
            if !path.is_empty() {
                return Ok(PathBuf::from(path));
            }
        }
    }
    Ok(home_dir()
        .ok_or("cannot determine home directory")?
        .join("Desktop"))
}

#[cfg(not(target_os = "macos"))]
fn copy_file_with_retry(from: &Path, to: &Path) -> Result<(), String> {
    let mut last_error = None;
    for _ in 0..40 {
        match fs::copy(from, to) {
            Ok(_) => return Ok(()),
            Err(error) => {
                last_error = Some(error);
                thread::sleep(Duration::from_millis(250));
            }
        }
    }
    Err(format!(
        "could not copy to {}: {}",
        to.display(),
        last_error.expect("at least one copy was attempted")
    ))
}

#[cfg(target_os = "macos")]
fn relaunch_companion(path: &Path) -> Result<(), String> {
    Command::new("open")
        .arg(path)
        .spawn()
        .map_err(|e| format!("could not relaunch {}: {e}", path.display()))?;
    Ok(())
}

#[cfg(not(target_os = "macos"))]
fn relaunch_companion(path: &Path) -> Result<(), String> {
    Command::new(path)
        .spawn()
        .map_err(|e| format!("could not relaunch {}: {e}", path.display()))?;
    Ok(())
}

#[cfg(target_os = "macos")]
fn schedule_mounted_volume_cleanup(path: Option<&Path>) {
    let Some(path) = path else {
        return;
    };
    // The installer is running from the mounted image, so detach it after this
    // process has had time to exit. Pass the path as a positional argument to
    // avoid interpolating it into the shell command.
    let _ = Command::new("sh")
        .args([
            "-c",
            "sleep 2; for attempt in 1 2 3 4 5; do if hdiutil detach \"$1\"; then rmdir \"$1\" 2>/dev/null; exit 0; fi; sleep 2; done",
            "scoretracker-cleanup",
        ])
        .arg(path)
        .spawn();
}

#[cfg(not(target_os = "macos"))]
fn schedule_mounted_volume_cleanup(_path: Option<&Path>) {}

// ---------------------------------------------------------------------------
// UI

enum Screen {
    Form,
    Installing {
        receiver: Receiver<InstallEvent>,
        progress: f32,
        message: String,
    },
    Done(InstallResult),
    Failed(String),
}

enum InstallEvent {
    Progress(f32, String),
    Finished(Result<InstallResult, String>),
}

struct InstallerApp {
    vpx: String,
    tables: String,
    create_desktop_icon: bool,
    update_mode: bool,
    automatic_update: bool,
    mounted_volume: Option<PathBuf>,
    close_requested: bool,
    screen: Screen,
}

impl InstallerApp {
    fn new(cc: &eframe::CreationContext<'_>, launch: LaunchOptions) -> Self {
        let mut visuals = egui::Visuals::light();
        visuals.panel_fill = egui::Color32::from_rgb(246, 247, 247);
        visuals.window_fill = egui::Color32::WHITE;
        visuals.faint_bg_color = egui::Color32::from_rgb(239, 242, 242);
        visuals.selection.bg_fill = egui::Color32::from_rgb(52, 59, 63);
        cc.egui_ctx.set_visuals(visuals);
        let mut style = (*cc.egui_ctx.style()).clone();
        style.spacing.item_spacing = egui::vec2(10.0, 10.0);
        style.spacing.button_padding = egui::vec2(16.0, 9.0);
        cc.egui_ctx.set_style(style);

        let remembered = read_install_config().unwrap_or_default();
        let create_desktop_icon =
            desktop_icon_preference(remembered.create_desktop_icon, launch.automatic_update);
        let update_mode = remembered.version.is_some();
        let vpx = remembered
            .vpx_root
            .filter(|path| Path::new(path).is_dir())
            .map(PathBuf::from)
            .or_else(detect_vpx)
            .map(|path| path.display().to_string())
            .unwrap_or_default();
        let tables = remembered
            .tables_root
            .filter(|path| Path::new(path).is_dir())
            .map(PathBuf::from)
            .unwrap_or_else(default_tables);
        let tables = if tables.as_os_str().is_empty() {
            String::new()
        } else {
            tables.display().to_string()
        };
        let mut app = Self {
            vpx,
            tables,
            create_desktop_icon,
            update_mode,
            automatic_update: launch.automatic_update,
            mounted_volume: launch.mounted_volume,
            close_requested: false,
            screen: Screen::Form,
        };
        if app.automatic_update {
            app.screen = match (
                Self::existing_dir(&app.vpx),
                Self::existing_dir(&app.tables),
            ) {
                (Some(vpx), Some(tables)) if resolve_plugins_dir(&vpx).is_some() => {
                    Self::install_screen(vpx, tables, true, app.create_desktop_icon)
                }
                _ => Screen::Failed(
                    "The saved VPX or tables folder is no longer available. Run the installer manually to choose the folders again."
                        .into(),
                ),
            };
        }
        app
    }

    fn install_screen(
        vpx: PathBuf,
        tables: PathBuf,
        update: bool,
        create_desktop_icon: bool,
    ) -> Screen {
        let (sender, receiver) = mpsc::channel();
        thread::spawn(move || {
            // Give the Viewer a moment to exit before its installed bundle or
            // executable is replaced, particularly on Windows.
            if update {
                thread::sleep(Duration::from_millis(750));
            }
            let result = install(&vpx, &tables, create_desktop_icon, |fraction, message| {
                let _ = sender.send(InstallEvent::Progress(fraction, message.to_owned()));
            });
            let _ = sender.send(InstallEvent::Finished(result));
        });
        Screen::Installing {
            receiver,
            progress: 0.0,
            message: if update {
                "Preparing the automatic update…".into()
            } else {
                "Preparing installation…".into()
            },
        }
    }

    fn form_ui(&mut self, ui: &mut egui::Ui) {
        ui.label(
            egui::RichText::new("SCORETRACKER SETUP")
                .strong()
                .size(12.0)
                .color(egui::Color32::from_rgb(82, 99, 107)),
        );
        ui.heading(if self.update_mode {
            "Review installation"
        } else {
            "Install ScoreTracker"
        });
        ui.label(
            egui::RichText::new(
                "Choose your Visual Pinball installation and tables library. ScoreTracker will install the plugin, maps, and Viewer together.",
            )
            .size(14.0)
            .color(egui::Color32::from_rgb(76, 85, 89)),
        );
        ui.add_space(8.0);
        ui.separator();
        ui.add_space(4.0);

        let plugins_dir = Self::existing_dir(&self.vpx).and_then(|dir| resolve_plugins_dir(&dir));
        ui.group(|ui| {
            ui.set_width(ui.available_width());
            ui.horizontal(|ui| {
                ui.label(
                    egui::RichText::new("01")
                        .strong()
                        .size(16.0)
                        .color(egui::Color32::from_rgb(82, 99, 107)),
                );
                ui.vertical(|ui| {
                    ui.label(egui::RichText::new("Visual Pinball X").strong().size(16.0));
                    ui.weak("Select the VPinballX application or installation folder.");
                });
            });
            Self::folder_row(ui, &mut self.vpx, "Choose the folder containing VPinballX");
            match &plugins_dir {
                Some(dir) => {
                    ui.colored_label(
                        egui::Color32::from_rgb(23, 105, 67),
                        format!("Ready · {}", dir.join("scoretracker").display()),
                    );
                }
                None if self.vpx.trim().is_empty() => {
                    ui.weak("Required · Choose the folder containing VPinballX.");
                }
                None => {
                    ui.colored_label(ui.visuals().error_fg_color, vpx_picker_help());
                }
            }
        });
        ui.add_space(8.0);

        let tables_dir = Self::existing_dir(&self.tables);
        ui.group(|ui| {
            ui.set_width(ui.available_width());
            ui.horizontal(|ui| {
                ui.label(
                    egui::RichText::new("02")
                        .strong()
                        .size(16.0)
                        .color(egui::Color32::from_rgb(82, 99, 107)),
                );
                ui.vertical(|ui| {
                    ui.label(egui::RichText::new("VPX tables").strong().size(16.0));
                    ui.weak("Choose the folder containing your .vpx table files.");
                });
            });
            Self::folder_row(ui, &mut self.tables, "Choose your VPX tables folder");
            if self.tables.trim().is_empty() {
                ui.weak("Required · Choose your tables folder.");
            } else if tables_dir.is_none() {
                ui.colored_label(ui.visuals().error_fg_color, "This folder does not exist.");
            } else {
                ui.colored_label(
                    egui::Color32::from_rgb(23, 105, 67),
                    "Ready · Completed games will be written beside each table.",
                );
            }
        });
        ui.add_space(8.0);

        ui.checkbox(&mut self.create_desktop_icon, "Create Desktop icon");
        ui.weak("Adds a shortcut for VPX Scoretracker Viewer to your Desktop.");
        ui.add_space(8.0);

        let ready = plugins_dir.is_some() && tables_dir.is_some();
        if ui
            .add_enabled(
                ready,
                egui::Button::new(
                    egui::RichText::new("Install")
                        .strong()
                        .size(15.0)
                        .color(egui::Color32::WHITE),
                )
                .fill(egui::Color32::from_rgb(52, 59, 63))
                .min_size(egui::vec2(ui.available_width(), 44.0)),
            )
            .clicked()
        {
            let vpx = Self::existing_dir(&self.vpx).expect("checked by `ready`");
            let tables = tables_dir.expect("checked by `ready`");
            self.screen = Self::install_screen(vpx, tables, false, self.create_desktop_icon);
        }
    }

    fn folder_row(ui: &mut egui::Ui, value: &mut String, title: &str) {
        ui.horizontal(|ui| {
            let field_width = (ui.available_width() - 112.0).max(180.0);
            ui.add_sized(
                [field_width, 34.0],
                egui::TextEdit::singleline(value).hint_text("Choose a folder…"),
            );
            let browse = ui.add_sized([102.0, 34.0], egui::Button::new("Browse…"));
            if browse.clicked() {
                let mut picker = rfd::FileDialog::new().set_title(title);
                if let Some(dir) = Self::existing_dir(value).or_else(home_dir) {
                    picker = picker.set_directory(dir);
                }
                if let Some(path) = picker.pick_folder() {
                    *value = path.display().to_string();
                }
            }
        });
    }

    fn existing_dir(value: &str) -> Option<PathBuf> {
        let trimmed = value.trim();
        if trimmed.is_empty() {
            return None;
        }
        let path = PathBuf::from(trimmed);
        path.is_dir().then_some(path)
    }

    fn installing_ui(&self, ui: &mut egui::Ui) {
        let Screen::Installing {
            progress, message, ..
        } = &self.screen
        else {
            return;
        };
        ui.vertical_centered(|ui| {
            ui.add_space(72.0);
            ui.label(
                egui::RichText::new(if self.automatic_update {
                    "AUTOMATIC UPDATE"
                } else {
                    "SCORETRACKER SETUP"
                })
                .strong()
                .size(12.0)
                .color(egui::Color32::from_rgb(82, 99, 107)),
            );
            ui.heading(if self.automatic_update {
                "Updating ScoreTracker"
            } else {
                "Installing ScoreTracker"
            });
            ui.add_space(12.0);
            ui.label(egui::RichText::new(message).size(14.0));
            ui.add_space(16.0);
            ui.add_sized(
                [440.0, 22.0],
                egui::ProgressBar::new(*progress)
                    .show_percentage()
                    .animate(true),
            );
            ui.add_space(12.0);
            ui.weak(if self.automatic_update {
                "The updated Viewer will reopen automatically."
            } else {
                "Please keep this window open while the files are installed."
            });
        });
    }

    fn poll_install(&mut self) {
        let events: Vec<InstallEvent> = match &self.screen {
            Screen::Installing { receiver, .. } => receiver.try_iter().collect(),
            _ => return,
        };
        for event in events {
            match event {
                InstallEvent::Progress(fraction, new_message) => {
                    if let Screen::Installing {
                        progress, message, ..
                    } = &mut self.screen
                    {
                        *progress = fraction;
                        *message = new_message;
                    }
                }
                InstallEvent::Finished(result) => {
                    self.screen = match result {
                        Ok(result) if self.automatic_update => {
                            let relaunch = result
                                .companion_path
                                .as_deref()
                                .ok_or_else(|| {
                                    "The Viewer payload could not be installed, so it cannot be relaunched."
                                        .to_owned()
                                })
                                .and_then(relaunch_companion);
                            match relaunch {
                                Ok(()) => {
                                    schedule_mounted_volume_cleanup(self.mounted_volume.as_deref());
                                    self.close_requested = true;
                                    Screen::Done(result)
                                }
                                Err(message) => Screen::Failed(message),
                            }
                        }
                        Ok(result) => Screen::Done(result),
                        Err(message) => Screen::Failed(message),
                    }
                }
            }
        }
    }

    fn result_ui(&self, ui: &mut egui::Ui, ctx: &egui::Context) {
        match &self.screen {
            Screen::Done(result) => {
                ui.label(
                    egui::RichText::new("INSTALLATION COMPLETE")
                        .strong()
                        .size(12.0)
                        .color(egui::Color32::from_rgb(23, 105, 67)),
                );
                ui.heading("ScoreTracker is ready");
                ui.add_space(12.0);
                ui.group(|ui| {
                    ui.set_width(ui.available_width());
                    ui.label(egui::RichText::new("Installed components").strong());
                    ui.weak(format!("Plugin · {}", result.plugin_dir.display()));
                    ui.weak(format!("Tables · {}", result.tables_dir.display()));
                    ui.weak(&result.companion_status);
                });
                ui.add_space(12.0);
                ui.label("Start a PinMAME table in VPX to begin recording scores.");
            }
            Screen::Failed(message) => {
                ui.label(
                    egui::RichText::new("INSTALLATION STOPPED")
                        .strong()
                        .size(12.0)
                        .color(ui.visuals().error_fg_color),
                );
                ui.heading("ScoreTracker could not be installed");
                ui.add_space(12.0);
                ui.group(|ui| {
                    ui.set_width(ui.available_width());
                    ui.colored_label(ui.visuals().error_fg_color, message);
                });
            }
            Screen::Form | Screen::Installing { .. } => {
                unreachable!("result_ui is only called for result screens")
            }
        }
        ui.add_space(12.0);
        if ui
            .add_sized([120.0, 40.0], egui::Button::new("Close"))
            .clicked()
        {
            schedule_mounted_volume_cleanup(self.mounted_volume.as_deref());
            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
        }
    }
}

impl eframe::App for InstallerApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.poll_install();
        egui::CentralPanel::default().show(ctx, |ui| {
            ui.add_space(18.0);
            ui.horizontal(|ui| {
                ui.add_space(24.0);
                ui.vertical(|ui| {
                    ui.set_max_width(640.0);
                    match &self.screen {
                        Screen::Form => self.form_ui(ui),
                        Screen::Installing { .. } => self.installing_ui(ui),
                        Screen::Done(_) | Screen::Failed(_) => self.result_ui(ui, ctx),
                    }
                });
            });
        });
        if self.close_requested {
            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
        }
        if matches!(&self.screen, Screen::Installing { .. }) {
            ctx.request_repaint_after(Duration::from_millis(50));
        }
    }
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
    return Some(location.join("Contents/PlugIns"));
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

fn read_install_config() -> Option<InstallConfig> {
    let path = companion_config_dir().ok()?.join("seed.json");
    let raw = fs::read_to_string(path).ok()?;
    serde_json::from_str(&raw).ok()
}

fn home_dir() -> Option<PathBuf> {
    std::env::var_os(if cfg!(windows) { "USERPROFILE" } else { "HOME" }).map(PathBuf::from)
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
            Some(app.join("Contents/PlugIns"))
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
            Some(app.join("Contents/PlugIns"))
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn installs_the_companion_app_bundle() {
        let root = test_root("companion");
        let bundle = root.join(format!("{COMPANION_APP_NAME}.app"));
        fs::create_dir_all(bundle.join("Contents/MacOS")).unwrap();
        fs::write(bundle.join("Contents/MacOS/viewer"), b"binary").unwrap();

        let apps = root.join("Applications");
        fs::create_dir_all(apps.join(format!("{LEGACY_COMPANION_APP_NAME}.app"))).unwrap();
        let dest = install_companion(&bundle, &apps).unwrap();
        assert_eq!(dest, apps.join(format!("{COMPANION_APP_NAME}.app")));
        assert!(dest.join("Contents/MacOS/viewer").is_file());
        assert!(!apps
            .join(format!("{LEGACY_COMPANION_APP_NAME}.app"))
            .exists());

        // Replaces an existing install rather than merging into it
        fs::write(dest.join("stale-file"), b"old").unwrap();
        let dest = install_companion(&bundle, &apps).unwrap();
        assert!(!dest.join("stale-file").exists());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn creates_and_removes_the_macos_desktop_shortcut() {
        let root = test_root("desktop-shortcut");
        let app = root.join(format!("Applications/{COMPANION_APP_NAME}.app"));
        fs::create_dir_all(&app).unwrap();
        let desktop = root.join("Desktop");
        let shortcut = desktop.join(format!("{COMPANION_APP_NAME}.app"));

        sync_macos_desktop_shortcut(&app, &desktop, true).unwrap();
        assert_eq!(fs::read_link(&shortcut).unwrap(), app);

        sync_macos_desktop_shortcut(&app, &desktop, true).unwrap();
        assert!(shortcut
            .symlink_metadata()
            .unwrap()
            .file_type()
            .is_symlink());

        sync_macos_desktop_shortcut(&app, &desktop, false).unwrap();
        assert!(!shortcut.exists());

        fs::create_dir_all(&shortcut).unwrap();
        assert!(sync_macos_desktop_shortcut(&app, &desktop, false).is_err());
        assert!(shortcut.is_dir());
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

    #[cfg(target_os = "linux")]
    #[test]
    fn installs_the_companion_binary_and_removes_the_old_appimage() {
        use std::os::unix::fs::PermissionsExt;

        let root = test_root("companion");
        let payload = root.join("payload/vpx-scoretracker-viewer");
        fs::create_dir_all(payload.parent().unwrap()).unwrap();
        fs::write(&payload, b"viewer").unwrap();

        let bin = root.join("bin");
        fs::create_dir_all(&bin).unwrap();
        fs::write(bin.join("vpx-scoretracker-viewer.AppImage"), b"old").unwrap();

        let dest = install_companion(&payload, &bin).unwrap();
        assert_eq!(dest, bin.join("vpx-scoretracker-viewer"));
        assert_eq!(fs::read(&dest).unwrap(), b"viewer");
        assert_eq!(dest.metadata().unwrap().permissions().mode() & 0o111, 0o111);
        assert!(!bin.join("vpx-scoretracker-viewer.AppImage").exists());
        fs::remove_dir_all(root).unwrap();
    }
}
