const RELEASE_API = "https://api.github.com/repos/herrMirto/vpx-scoretracker-plugin/releases/latest";

function formatSize(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) return "";
  const megabytes = bytes / (1024 * 1024);
  return `${megabytes >= 10 ? megabytes.toFixed(0) : megabytes.toFixed(1)} MB`;
}

async function resolveLatestDownloads() {
  const status = document.querySelector("#release-status");
  const links = [...document.querySelectorAll("[data-asset]")];

  try {
    const response = await fetch(RELEASE_API, {
      headers: { Accept: "application/vnd.github+json" },
    });
    if (!response.ok) throw new Error(`GitHub returned ${response.status}`);

    const release = await response.json();
    const assets = new Map((release.assets ?? []).map((asset) => [asset.name, asset]));
    const version = String(release.tag_name ?? "").replace(/^v/, "");

    for (const link of links) {
      const asset = assets.get(link.dataset.asset);
      if (!asset) continue;
      link.href = asset.browser_download_url;
      const meta = link.querySelector("small");
      const size = formatSize(asset.size);
      if (meta && version) meta.textContent = `${meta.textContent} · v${version}${size ? ` · ${size}` : ""}`;
    }

    if (status) status.textContent = version ? `Latest version · ${version}` : "Latest release";
  } catch {
    if (status) status.textContent = "Latest release · direct links ready";
  }
}

resolveLatestDownloads();
