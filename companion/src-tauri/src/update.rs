use std::fs::File;
use std::io::Write;
use std::path::Path;
use std::process::Command;
use std::time::Duration;

use reqwest::header::{ACCEPT, USER_AGENT};
use semver::Version;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use tauri::Manager;

const REPOSITORY: &str = "herrMirto/vpx-scoretracker-plugin";
const LATEST_RELEASE_URL: &str =
    "https://api.github.com/repos/herrMirto/vpx-scoretracker-plugin/releases/latest";
const DOWNLOAD_PREFIX: &str =
    "https://github.com/herrMirto/vpx-scoretracker-plugin/releases/download/";

#[derive(Debug, Deserialize)]
struct GithubRelease {
    tag_name: String,
    html_url: String,
    body: Option<String>,
    published_at: Option<String>,
    assets: Vec<GithubAsset>,
}

#[derive(Debug, Deserialize)]
struct GithubAsset {
    name: String,
    browser_download_url: String,
    digest: Option<String>,
    size: u64,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateInfo {
    version: String,
    current_version: String,
    release_url: String,
    release_notes: String,
    published_at: Option<String>,
    asset_name: String,
    download_url: String,
    digest: String,
    size: u64,
}

fn client() -> Result<reqwest::Client, String> {
    reqwest::Client::builder()
        .timeout(Duration::from_secs(30))
        .build()
        .map_err(|error| format!("could not create update client: {error}"))
}

fn expected_asset_name() -> Result<String, String> {
    match (std::env::consts::OS, std::env::consts::ARCH) {
        ("windows", "x86_64") => Ok("scoretracker-installer-windows-x64.exe".into()),
        ("linux", "x86_64") => Ok("scoretracker-installer-linux-x64".into()),
        ("linux", "aarch64") => Ok("scoretracker-installer-linux-arm64".into()),
        ("macos", "aarch64") => Ok("scoretracker-installer-macos-arm64.dmg".into()),
        (os, arch) => Err(format!("updates are not published for {os}/{arch}")),
    }
}

fn parse_release_version(tag: &str) -> Result<Version, String> {
    Version::parse(tag.trim_start_matches(|character| character == 'v' || character == 'V'))
        .map_err(|error| format!("release tag {tag:?} is not a valid version: {error}"))
}

fn select_update(
    release: GithubRelease,
    current: &Version,
    asset_name: &str,
) -> Result<Option<UpdateInfo>, String> {
    let latest = parse_release_version(&release.tag_name)?;
    if latest <= *current {
        return Ok(None);
    }

    let asset = release
        .assets
        .into_iter()
        .find(|asset| asset.name == asset_name)
        .ok_or_else(|| format!("release {} has no {asset_name} installer", release.tag_name))?;
    let digest = asset
        .digest
        .ok_or_else(|| format!("GitHub did not provide a SHA-256 digest for {}", asset.name))?;

    Ok(Some(UpdateInfo {
        version: latest.to_string(),
        current_version: current.to_string(),
        release_url: release.html_url,
        release_notes: release.body.unwrap_or_default(),
        published_at: release.published_at,
        asset_name: asset.name,
        download_url: asset.browser_download_url,
        digest,
        size: asset.size,
    }))
}

#[tauri::command]
pub async fn check_for_update() -> Result<Option<UpdateInfo>, String> {
    let response = client()?
        .get(LATEST_RELEASE_URL)
        .header(
            USER_AGENT,
            format!("VPX-Scoretracker-Viewer/{}", env!("CARGO_PKG_VERSION")),
        )
        .header(ACCEPT, "application/vnd.github+json")
        .send()
        .await
        .map_err(|error| format!("could not check GitHub Releases: {error}"))?;
    if response.status() == reqwest::StatusCode::NOT_FOUND {
        return Ok(None);
    }
    let response = response
        .error_for_status()
        .map_err(|error| format!("GitHub Releases returned an error: {error}"))?;
    let release: GithubRelease = response
        .json()
        .await
        .map_err(|error| format!("could not read the GitHub release: {error}"))?;

    let current = Version::parse(env!("CARGO_PKG_VERSION"))
        .map_err(|error| format!("invalid installed version: {error}"))?;
    let asset_name = expected_asset_name()?;
    select_update(release, &current, &asset_name)
}

fn validate_update(update: &UpdateInfo) -> Result<(), String> {
    let expected_name = expected_asset_name()?;
    if update.asset_name != expected_name {
        return Err(format!(
            "refusing unexpected update asset {}; expected {expected_name}",
            update.asset_name
        ));
    }
    if !update.download_url.starts_with(DOWNLOAD_PREFIX) {
        return Err(format!(
            "refusing update URL outside the {REPOSITORY} release page"
        ));
    }
    if !update.digest.starts_with("sha256:") {
        return Err("the update does not have a GitHub SHA-256 digest".into());
    }
    Ok(())
}

async fn download(update: &UpdateInfo, destination: &Path) -> Result<(), String> {
    let mut response = client()?
        .get(&update.download_url)
        .header(
            USER_AGENT,
            format!("VPX-Scoretracker-Viewer/{}", env!("CARGO_PKG_VERSION")),
        )
        .send()
        .await
        .map_err(|error| format!("could not download the update: {error}"))?
        .error_for_status()
        .map_err(|error| format!("update download returned an error: {error}"))?;

    let mut file = File::create(destination)
        .map_err(|error| format!("could not create {}: {error}", destination.display()))?;
    let mut hasher = Sha256::new();
    let mut downloaded = 0_u64;
    while let Some(chunk) = response
        .chunk()
        .await
        .map_err(|error| format!("update download was interrupted: {error}"))?
    {
        file.write_all(&chunk)
            .map_err(|error| format!("could not write {}: {error}", destination.display()))?;
        hasher.update(&chunk);
        downloaded += chunk.len() as u64;
    }
    file.flush()
        .map_err(|error| format!("could not finish {}: {error}", destination.display()))?;

    if downloaded != update.size {
        return Err(format!(
            "downloaded {downloaded} bytes, but GitHub reported {}",
            update.size
        ));
    }
    let actual = format!("sha256:{:x}", hasher.finalize());
    if actual != update.digest.to_ascii_lowercase() {
        return Err("the downloaded installer failed SHA-256 verification".into());
    }
    Ok(())
}

#[cfg(target_os = "linux")]
fn launch_installer(path: &Path) -> Result<(), String> {
    use std::os::unix::fs::PermissionsExt;

    std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o755))
        .map_err(|error| format!("could not mark {} executable: {error}", path.display()))?;
    Command::new(path)
        .spawn()
        .map_err(|error| format!("could not launch {}: {error}", path.display()))?;
    Ok(())
}

#[cfg(target_os = "windows")]
fn launch_installer(path: &Path) -> Result<(), String> {
    Command::new(path)
        .spawn()
        .map_err(|error| format!("could not launch {}: {error}", path.display()))?;
    Ok(())
}

#[cfg(target_os = "macos")]
fn launch_installer(path: &Path) -> Result<(), String> {
    Command::new("open")
        .arg(path)
        .spawn()
        .map_err(|error| format!("could not open {}: {error}", path.display()))?;
    Ok(())
}

#[tauri::command]
pub async fn download_and_launch_update(
    app: tauri::AppHandle,
    update: UpdateInfo,
) -> Result<(), String> {
    validate_update(&update)?;
    let cache_dir = app
        .path()
        .app_cache_dir()
        .map_err(|error| format!("could not locate the update cache: {error}"))?;
    std::fs::create_dir_all(&cache_dir)
        .map_err(|error| format!("could not create {}: {error}", cache_dir.display()))?;
    let destination = cache_dir.join(&update.asset_name);
    download(&update, &destination).await?;
    launch_installer(&destination)?;

    // The installer may need to replace the Viewer executable or app bundle.
    // Exit only after the verified updater process has started successfully.
    app.exit(0);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn release(version: &str) -> GithubRelease {
        GithubRelease {
            tag_name: format!("v{version}"),
            html_url: format!("https://github.com/{REPOSITORY}/releases/tag/v{version}"),
            body: Some("Release notes".into()),
            published_at: Some("2026-07-19T00:00:00Z".into()),
            assets: vec![GithubAsset {
                name: expected_asset_name().unwrap(),
                browser_download_url: format!(
                    "{DOWNLOAD_PREFIX}v{version}/{}",
                    expected_asset_name().unwrap()
                ),
                digest: Some(format!("sha256:{}", "a".repeat(64))),
                size: 42,
            }],
        }
    }

    #[test]
    fn parses_prefixed_release_versions() {
        assert_eq!(
            parse_release_version("v0.2.1").unwrap(),
            Version::new(0, 2, 1)
        );
    }

    #[test]
    fn selects_a_newer_platform_release() {
        let update = select_update(
            release("0.2.0"),
            &Version::new(0, 1, 0),
            &expected_asset_name().unwrap(),
        )
        .unwrap()
        .unwrap();
        assert_eq!(update.version, "0.2.0");
        assert_eq!(update.current_version, "0.1.0");
        assert_eq!(update.size, 42);
    }

    #[test]
    fn ignores_the_installed_release() {
        let update = select_update(
            release("0.1.0"),
            &Version::new(0, 1, 0),
            &expected_asset_name().unwrap(),
        )
        .unwrap();
        assert!(update.is_none());
    }

    #[test]
    fn rejects_downloads_from_another_repository() {
        let update = UpdateInfo {
            version: "0.2.0".into(),
            current_version: "0.1.0".into(),
            release_url: String::new(),
            release_notes: String::new(),
            published_at: None,
            asset_name: expected_asset_name().unwrap(),
            download_url: "https://example.com/update".into(),
            digest: format!("sha256:{}", "0".repeat(64)),
            size: 1,
        };
        assert!(validate_update(&update).is_err());
    }
}
