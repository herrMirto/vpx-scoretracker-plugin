const RELEASE_API = "https://api.github.com/repos/herrMirto/vpx-scoretracker-plugin/releases/latest";
const RELEASES_URL = "https://github.com/herrMirto/vpx-scoretracker-plugin/releases";

function formatSize(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) return "";
  const megabytes = bytes / (1024 * 1024);
  return `${megabytes >= 10 ? megabytes.toFixed(0) : megabytes.toFixed(1)} MB`;
}

function safeAssetUrl(value) {
  try {
    const url = new URL(value);
    return url.origin === "https://github.com" && url.pathname.toLowerCase().startsWith("/herrmirto/vpx-scoretracker-plugin/releases/download/") ? url.href : null;
  } catch { return null; }
}

async function resolveLatestDownloads() {
  const status = document.querySelector("#release-status");
  const links = [...document.querySelectorAll("[data-asset-template]")];
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 8000);
  try {
    const response = await fetch(RELEASE_API, {signal: controller.signal, headers: {Accept: "application/vnd.github+json"}});
    if (!response.ok) throw new Error(`GitHub returned ${response.status}`);
    const release = await response.json();
    const version = String(release.tag_name ?? "").replace(/^v/, "");
    if (!version) throw new Error("Release version missing");
    const assets = new Map((release.assets ?? []).map(asset => [asset.name, asset]));
    for (const link of links) {
      const name = link.dataset.assetTemplate.replace("{version}", version);
      const asset = assets.get(name);
      const url = safeAssetUrl(asset?.browser_download_url);
      if (url) {
        link.href = url;
        link.textContent = `${link.dataset.kind} ZIP`;
        link.dataset.available = "true";
        const size = formatSize(asset.size);
        link.title = `${name}${size ? ` · ${size}` : ""}`;
      } else {
        link.href = RELEASES_URL;
        link.textContent = `${link.dataset.kind} releases`;
        link.dataset.available = "false";
      }
    }
    for (const label of document.querySelectorAll("[data-platform-status]")) {
      const platform = label.dataset.platformStatus;
      const platformLinks = links.filter(link => link.dataset.assetTemplate.endsWith(`-${platform}.zip`));
      const ready = platformLinks.filter(link => link.dataset.available === "true").length;
      label.textContent = ready === 2 ? `v${version} · ZIP` : ready === 0 ? `v${version} · ZIPs not published` : `v${version} · one ZIP not published`;
    }
    if (status) status.textContent = `Latest release: v${version}`;
  } catch {
    if (status) status.textContent = "View releases on GitHub";
    for (const link of links) {
      link.href = RELEASES_URL;
      link.textContent = `${link.dataset.kind} releases`;
    }
    for (const label of document.querySelectorAll("[data-platform-status]")) label.textContent = "Download from GitHub releases";
  } finally { clearTimeout(timeout); }
}

resolveLatestDownloads();
