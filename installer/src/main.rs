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
use std::sync::mpsc::{self, Receiver};
use std::thread;
use std::time::Duration;

use eframe::egui;
use flate2::read::GzDecoder;

const COMPANION_ID: &str = "com.antigravity.scoretracker.companion";
const COMPANION_APP_NAME: &str = "VPX Scoretracker Viewer";
const LEGACY_COMPANION_APP_NAME: &str = "VPX Scoretracker Visualiser";
const EMBEDDED_PAYLOAD: &[u8] =
    include_bytes!(concat!(env!("OUT_DIR"), "/scoretracker-payload.tar.gz"));

fn main() -> Result<(), eframe::Error> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([620.0, 330.0])
            .with_min_inner_size([620.0, 330.0]),
        centered: true,
        ..Default::default()
    };
    eframe::run_native(
        "ScoreTracker Installer",
        options,
        Box::new(|_cc| Ok(Box::new(InstallerApp::new()))),
    )
}

#[derive(Debug)]
struct InstallResult {
    plugin_dir: PathBuf,
    tables_dir: PathBuf,
    companion_status: String,
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
    let json = format!(
        "{{\n  \"tablesRoot\": \"{}\",\n  \"mapsRoot\": \"{}\"\n}}\n",
        json_escape(&tables.display().to_string()),
        json_escape(&dest.join("maps").display().to_string())
    );
    fs::write(&seed, json).map_err(|e| format!("could not write {}: {e}", seed.display()))?;

    progress(0.78, "Installing VPX Scoretracker Viewer…");
    // A failed Viewer copy must not roll back the plugin install that already
    // succeeded, but the completion screen makes the failure explicit.
    let companion_status = match companion_payload(extracted.path()) {
        None => format!("{COMPANION_APP_NAME} is not bundled with this installer."),
        Some(bundle) => {
            match companion_dest_root().and_then(|root| install_companion(&bundle, &root)) {
                Ok(installed) => format!("Viewer: {}", installed.display()),
                Err(e) => format!("{COMPANION_APP_NAME} could not be installed: {e}"),
            }
        }
    };

    progress(1.0, "Installation complete");
    Ok(InstallResult {
        plugin_dir: dest,
        tables_dir: tables.to_path_buf(),
        companion_status,
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
    fs::copy(exe, &dest).map_err(|e| format!("could not copy to {}: {e}", dest.display()))?;
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
    fs::copy(binary, &dest).map_err(|e| format!("could not copy to {}: {e}", dest.display()))?;
    fs::set_permissions(&dest, fs::Permissions::from_mode(0o755))
        .map_err(|e| format!("could not mark {} executable: {e}", dest.display()))?;
    let _ = fs::remove_file(bin_dir.join("vpx-scoretracker-viewer.AppImage"));
    let _ = fs::remove_file(bin_dir.join("vpx-scoretracker-visualiser.AppImage"));
    // Desktop entry, best effort: the binary runs from ~/.local/bin either way
    if let Some(home) = home_dir() {
        let apps = home.join(".local/share/applications");
        if fs::create_dir_all(&apps).is_ok() {
            let _ = fs::remove_file(apps.join("vpx-scoretracker-visualiser.desktop"));
            let entry = format!(
                "[Desktop Entry]\nType=Application\nName={COMPANION_APP_NAME}\nExec=\"{}\"\nCategories=Game;\nTerminal=false\n",
                dest.display()
            );
            let _ = fs::write(apps.join("vpx-scoretracker-viewer.desktop"), entry);
        }
    }
    Ok(dest)
}

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
    screen: Screen,
}

impl InstallerApp {
    /// Detection only prefills the form fields; nothing is installed until the
    /// user has both paths on screen and clicks Install.
    fn new() -> Self {
        let vpx = detect_vpx()
            .map(|path| path.display().to_string())
            .unwrap_or_default();
        let tables = default_tables();
        let tables = if tables.as_os_str().is_empty() {
            String::new()
        } else {
            tables.display().to_string()
        };
        Self {
            vpx,
            tables,
            screen: Screen::Form,
        }
    }

    fn form_ui(&mut self, ui: &mut egui::Ui) {
        ui.heading("Install ScoreTracker");
        ui.add_space(8.0);

        ui.label("VPinballX folder:");
        Self::folder_row(ui, &mut self.vpx, "Choose the folder containing VPinballX");
        let plugins_dir = Self::existing_dir(&self.vpx).and_then(|dir| resolve_plugins_dir(&dir));
        if self.vpx.trim().is_empty() {
            ui.weak("Select the folder containing VPinballX.");
        } else {
            match &plugins_dir {
                Some(dir) => {
                    ui.weak(format!(
                        "Plugin will be installed to {}",
                        dir.join("scoretracker").display()
                    ));
                }
                None => {
                    ui.colored_label(ui.visuals().error_fg_color, vpx_picker_help());
                }
            }
        }
        ui.add_space(8.0);

        ui.label("VPX tables folder:");
        Self::folder_row(ui, &mut self.tables, "Choose your VPX tables folder");
        let tables_dir = Self::existing_dir(&self.tables);
        if self.tables.trim().is_empty() {
            ui.weak("Select the folder containing your .vpx tables.");
        } else if tables_dir.is_none() {
            ui.colored_label(ui.visuals().error_fg_color, "This folder does not exist.");
        } else {
            ui.weak("Completed games are written to scores.json next to each table.");
        }
        ui.add_space(12.0);

        let ready = plugins_dir.is_some() && tables_dir.is_some();
        if ui
            .add_enabled(ready, egui::Button::new("Install"))
            .clicked()
        {
            let vpx = Self::existing_dir(&self.vpx).expect("checked by `ready`");
            let tables = tables_dir.expect("checked by `ready`");
            let (sender, receiver) = mpsc::channel();
            thread::spawn(move || {
                let result = install(&vpx, &tables, |fraction, message| {
                    let _ = sender.send(InstallEvent::Progress(fraction, message.to_owned()));
                });
                let _ = sender.send(InstallEvent::Finished(result));
            });
            self.screen = Screen::Installing {
                receiver,
                progress: 0.0,
                message: "Preparing installation…".into(),
            };
        }
    }

    fn folder_row(ui: &mut egui::Ui, value: &mut String, title: &str) {
        ui.horizontal(|ui| {
            let browse = ui.button("Browse\u{2026}");
            ui.add_sized(
                [ui.available_width(), 20.0],
                egui::TextEdit::singleline(value),
            );
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
        ui.heading("Installing ScoreTracker");
        ui.add_space(16.0);
        ui.label(message);
        ui.add_space(8.0);
        ui.add(
            egui::ProgressBar::new(*progress)
                .show_percentage()
                .animate(true),
        );
        ui.add_space(8.0);
        ui.weak("Please keep this window open while the files are installed.");
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
                        Ok(result) => Screen::Done(result),
                        Err(message) => Screen::Failed(message),
                    };
                }
            }
        }
    }

    fn result_ui(&self, ui: &mut egui::Ui, ctx: &egui::Context) {
        match &self.screen {
            Screen::Done(result) => {
                ui.heading("ScoreTracker installed");
                ui.add_space(8.0);
                ui.label(format!("Plugin: {}", result.plugin_dir.display()));
                ui.label(format!("Tables: {}", result.tables_dir.display()));
                ui.label(&result.companion_status);
                ui.add_space(8.0);
                ui.label("Start a PinMAME table in VPX to begin recording scores.");
            }
            Screen::Failed(message) => {
                ui.heading("ScoreTracker could not be installed");
                ui.add_space(8.0);
                ui.colored_label(ui.visuals().error_fg_color, message);
            }
            Screen::Form | Screen::Installing { .. } => {
                unreachable!("result_ui is only called for result screens")
            }
        }
        ui.add_space(12.0);
        if ui.button("Close").clicked() {
            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
        }
    }
}

impl eframe::App for InstallerApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.poll_install();
        egui::CentralPanel::default().show(ctx, |ui| match &self.screen {
            Screen::Form => self.form_ui(ui),
            Screen::Installing { .. } => self.installing_ui(ui),
            Screen::Done(_) | Screen::Failed(_) => self.result_ui(ui, ctx),
        });
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
