import { invoke } from "@tauri-apps/api/core";
import { open } from "@tauri-apps/plugin-dialog";
import "./styles.css";
import type { GameRecord, NvramDocument, ScanSnapshot, UpdateInfo } from "./types";

const TABLES_ROOT_KEY = "scoretracker.tablesRoot";
const MAPS_ROOT_KEY = "scoretracker.mapsRoot";
const MEDIA_CACHE_KEY = "scoretracker.tableMedia.v1";
const UPDATE_CHECK_KEY = "scoretracker.lastUpdateCheck";
const UPDATE_CHECK_INTERVAL = 24 * 60 * 60 * 1000;
const VPINPLAY_API_BASE = "https://api.vpinplay.com:8888/api/v1";
const VPINMEDIA_BASE = "https://raw.githubusercontent.com/superhac/vpinmediadb/refs/heads/main";
const MEDIA_CACHE_MAX_AGE = 30 * 24 * 60 * 60 * 1000;
const app = document.querySelector<HTMLDivElement>("#app")!;

if (!app) throw new Error("Application root was not found");

interface ScoreEntry {
  score: number;
  date: string;
  duration: number | null;
  game: GameRecord;
}

interface TableHistory {
  rom: string;
  name: string;
  games: GameRecord[];
  entries: ScoreEntry[];
  best: number;
  average: number;
  totalTime: number;
  latest: ScoreEntry;
  vpxFileName: string | null;
  vpxFileHash: string | null;
}

interface TableMedia {
  vpsId: string;
  name: string;
  manufacturer: string;
  year: string;
  wheelUrl: string;
  resolvedAt: number;
}

interface VPinPlayItem {
  vpsId?: string;
  name?: string;
  manufacturer?: string;
  year?: string | number;
  filename?: string;
  filehash?: string;
  vpsdb?: { name?: string; manufacturer?: string; year?: string | number };
}

interface ChartTarget {
  score: number;
  label: string;
  name: string;
  initials: string;
  rank: number;
}

let snapshot: ScanSnapshot | null = null;
let busy = false;
let fatalError = "";
let nvram: NvramDocument | null = null;
let nvramRom = "";
let nvramBusy = false;
let nvramError = "";
const tableMedia = new Map<string, TableMedia>();
let mediaGeneration = "";
let foldersOpen = false;
let availableUpdate: UpdateInfo | null = null;
let updateBusy = false;
let updateStatus = "";

function number(value: number): string {
  return new Intl.NumberFormat().format(value);
}

function duration(seconds: number | null): string {
  if (seconds === null) return "—";
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  return hours > 0 ? `${hours}h ${minutes}m` : `${minutes}m`;
}

function date(value: string, includeTime = true): string {
  const parsed = new Date(value);
  if (Number.isNaN(parsed.getTime())) return value || "Unknown date";
  return new Intl.DateTimeFormat(undefined, includeTime
    ? { dateStyle: "medium", timeStyle: "short" }
    : { month: "short", day: "numeric" }).format(parsed);
}

function esc(value: unknown): string {
  const replacements: Record<string, string> = {
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  };
  return String(value ?? "").replace(/[&<>"']/g, (char) => replacements[char] ?? char);
}

function tables(): TableHistory[] {
  const grouped = new Map<string, { name: string; games: GameRecord[] }>();
  for (const game of snapshot?.games ?? []) {
    if (!game.scores.some((score) => score > 0)) continue;
    const key = game.rom || game.table;
    const group = grouped.get(key) ?? { name: game.table || game.rom, games: [] };
    group.games.push(game);
    grouped.set(key, group);
  }

  return [...grouped.entries()].map(([rom, group]) => {
    group.games.sort((left, right) => left.date.localeCompare(right.date));
    const entries = group.games.flatMap((game) => game.scores
      .filter((score) => score > 0)
      .map((score) => ({ score, date: game.date, duration: game.gameDuration, game })));
    const best = Math.max(...entries.map((entry) => entry.score));
    return {
      rom,
      name: group.name,
      games: group.games,
      entries,
      best,
      average: Math.round(entries.reduce((sum, entry) => sum + entry.score, 0) / entries.length),
      totalTime: group.games.reduce((sum, game) => sum + (game.gameDuration ?? 0), 0),
      latest: entries[entries.length - 1],
      vpxFileName: entries[entries.length - 1].game.vpxFileName,
      vpxFileHash: entries[entries.length - 1].game.vpxFileHash,
    };
  }).sort((left, right) => right.latest.date.localeCompare(left.latest.date));
}

function selectedRom(): string | null {
  const match = location.hash.match(/^#\/table\/(.+)$/);
  return match ? decodeURIComponent(match[1]) : null;
}

function render(): void {
  const root = localStorage.getItem(TABLES_ROOT_KEY) ?? "";
  const mapsRoot = localStorage.getItem(MAPS_ROOT_KEY) ?? "";
  const tableList = tables();
  const selected = selectedRom();
  const table = selected ? tableList.find((candidate) => candidate.rom === selected) : null;
  const configured = Boolean(root);

  app.innerHTML = `
    ${renderTopbar(root, Boolean(table))}
    <main>
      ${renderNotices()}
      ${table && configured ? renderTableDetail(table) : configured ? renderOverview(tableList) : renderSetup(root)}
    </main>
    ${foldersOpen && configured ? renderFoldersModal(root) : ""}`;

  wireEvents();
  if (configured && tableList.length) void ensureTableMedia(tableList);
  if (table && configured) void ensureNvram(table, root, mapsRoot);
}

function renderTopbar(root: string, inDetail: boolean): string {
  return `<header class="topbar">
    <button class="brand brand-button" id="home" type="button" aria-label="ScoreTracker home">
      <span class="brand-stripe" aria-hidden="true"></span><span class="brand-copy"><strong>VPX ScoreTracker</strong><small>Local scores</small></span>
    </button>
    <nav class="actions" aria-label="Application actions">
      ${inDetail ? `<button id="back" class="button secondary" type="button">← All tables</button>` : ""}
      ${root ? `<button id="show-folders" class="button secondary" type="button">Tables Folder</button>` : ""}
      ${root ? `<button id="refresh" class="button primary" type="button" ${busy ? "disabled" : ""}>${busy ? "Scanning…" : "Refresh scores"}</button>` : ""}
      <button id="check-update" class="button ${availableUpdate ? "update-nav" : "secondary"}" type="button" ${updateBusy ? "disabled" : ""}>${updateBusy ? "Checking…" : availableUpdate ? `Update ${esc(availableUpdate.version)}` : "Check for Updates"}</button>
    </nav>
  </header>`;
}

function renderNotices(): string {
  const notices: string[] = [];
  if (availableUpdate) {
    notices.push(`<section class="update-card" role="alert" aria-live="polite">
      <div class="update-mark" aria-hidden="true">↑</div>
      <div class="update-copy">
        <p class="update-eyebrow">Software update</p>
        <strong>ScoreTracker ${esc(availableUpdate.version)} is ready</strong>
        <span>${esc(formatBytes(availableUpdate.size))} · Updates the Viewer, VPX plugin, and maps, then restarts automatically. Close VPX before continuing.</span>
      </div>
      <button id="install-update" class="button update-action" type="button" ${updateBusy ? "disabled" : ""}>${updateBusy ? "Downloading and verifying…" : "Update and restart"}</button>
    </section>`);
  } else if (updateStatus) {
    notices.push(`<div class="notice update-status" role="status">${esc(updateStatus)}</div>`);
  }
  if (fatalError) notices.push(`<div class="notice error" role="alert">${esc(fatalError)}</div>`);
  if (snapshot?.warnings.length) {
    notices.push(`<details class="notice warning"><summary>${snapshot.warnings.length} source warning(s)</summary><ul>${snapshot.warnings.map((warning) => `<li><strong>${esc(warning.source)}</strong>: ${esc(warning.message)}</li>`).join("")}</ul></details>`);
  }
  return notices.join("");
}

function formatBytes(bytes: number): string {
  if (bytes < 1024 * 1024) return `${Math.max(1, Math.round(bytes / 1024))} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function renderOverview(tableList: TableHistory[]): string {
  const games = snapshot?.games ?? [];
  const totalTime = games.reduce((sum, game) => sum + (game.gameDuration ?? 0), 0);

  return `<section class="stats" aria-label="History summary">
      ${stat("Completed games", number(games.length), "Valid scores recorded")}
      ${stat("Played tables", number(tableList.length), "Unique ROMs")}
      ${stat("Tables found", number(snapshot?.vpxFilesFound ?? 0), "VPX files discovered")}
      ${stat("Recorded time", duration(totalTime), "Across all sessions")}
    </section>
    <section class="section-heading"><div><p class="eyebrow">Recently played</p><h2>Table progress</h2></div><span>Most recent first · Select a table for its complete history</span></section>
    ${tableList.length ? `<section class="table-grid">${tableList.map(renderTableCard).join("")}</section>` : renderEmptyHistory()}`;
}

function renderTableCard(table: TableHistory): string {
  const recent = table.entries.slice(-6);
  const points = sparklinePoints(recent.map((entry) => entry.score), 240, 58, 5);
  return `<button class="table-card" type="button" data-rom="${esc(table.rom)}">
    <div class="table-card-art">${renderWheel(table, "card-wheel")}${tableMedia.has(table.rom) ? `<span class="media-credit">VPinMediaDB</span>` : ""}</div>
    <div class="table-card-head"><span class="table-index">Played ${date(table.latest.date, false)}</span><span class="game-count">${table.entries.length} game${table.entries.length === 1 ? "" : "s"}</span></div>
    <div class="table-card-copy"><h3>${esc(displayName(table))}</h3>${renderMediaMeta(table)}<code>ROM / ${esc(table.rom)}</code></div>
    <svg class="sparkline" viewBox="0 0 240 58" role="img" aria-label="Recent score trend for ${esc(table.name)}">
      ${recent.length > 1 ? `<path class="spark-area" d="${areaPath(points, 58)}"></path><polyline points="${points}" vector-effect="non-scaling-stroke"></polyline>` : ""}
      <g class="spark-dots">${scorePointCircles(recent, points)}</g>
    </svg>
    <div class="table-card-foot"><span><small>Personal best</small><strong>${number(table.best)}</strong></span><span class="closeness"><small>Latest score</small><strong>${number(table.latest.score)}</strong></span></div>
  </button>`;
}

function renderWheel(table: TableHistory, className: string): string {
  const media = tableMedia.get(table.rom);
  return `<div class="wheel-art ${className}" data-wheel-rom="${esc(table.rom)}">
    <span class="wheel-fallback">${esc(displayName(table))}</span>
    ${media ? `<img class="wheel-image" src="${esc(media.wheelUrl)}" alt="${esc(media.name)} wheel artwork" loading="lazy">` : ""}
  </div>`;
}

function displayName(table: TableHistory): string {
  return tableMedia.get(table.rom)?.name || table.name || table.rom;
}

function renderMediaMeta(table: TableHistory): string {
  const media = tableMedia.get(table.rom);
  if (!media) return "";
  const details = [media.manufacturer, media.year].filter(Boolean).join(" · ");
  return details ? `<span class="table-meta">${esc(details)}</span>` : "";
}

function detailEyebrow(table: TableHistory): string {
  const media = tableMedia.get(table.rom);
  const identity = media ? [media.manufacturer, media.year].filter(Boolean).join(" · ") : "PinMAME table";
  return `${identity} · ${table.rom}`;
}

async function ensureTableMedia(tableList: TableHistory[]): Promise<void> {
  const generation = `${snapshot?.generatedAt ?? ""}|${tableList.map(mediaCacheKey).join(";")}`;
  if (mediaGeneration === generation) return;
  mediaGeneration = generation;

  const cache = readMediaCache();
  const pending: TableHistory[] = [];
  for (const table of tableList) {
    const cached = cache[mediaCacheKey(table)];
    if (cached && Date.now() - cached.resolvedAt < MEDIA_CACHE_MAX_AGE) {
      tableMedia.set(table.rom, cached);
    } else {
      pending.push(table);
    }
  }

  for (let index = 0; index < pending.length; index += 4) {
    const chunk = pending.slice(index, index + 4);
    const resolved = await Promise.all(chunk.map(async (table) => ({ table, media: await resolveTableMedia(table) })));
    for (const { table, media } of resolved) {
      if (!media) continue;
      tableMedia.set(table.rom, media);
      cache[mediaCacheKey(table)] = media;
    }
  }

  writeMediaCache(cache);
  if (mediaGeneration === generation) render();
}

async function resolveTableMedia(table: TableHistory): Promise<TableMedia | null> {
  // The initial scan intentionally does not hash full VPX files. Most wheels can
  // be resolved from the table/VPX name without doing heavy disk I/O.
  const query = table.name.trim();
  if (query) {
    const search = await fetchVPinPlay<{ items?: VPinPlayItem[] }>(
      `/vpsdb/search?q=${encodeURIComponent(query)}&limit=30`,
    );
    const best = pickBestVPinPlayItem(search?.items ?? [], table);
    if (best?.vpsId) return mediaFromItem(best, best.vpsId);
  }

  if (table.rom) {
    const romMatches = await fetchVPinPlay<{ items?: VPinPlayItem[] }>(
      `/tables/by-rom/${encodeURIComponent(table.rom)}?limit=100`,
    );
    const items = romMatches?.items ?? [];
    const best = pickBestVPinPlayItem(items, table) ?? pickDominantVpsItem(items);
    if (best?.vpsId) return mediaFromItem(best, best.vpsId);
  }

  // Exact file hashing is an expensive last resort. It runs in a blocking worker
  // on the Rust side, after local scores are already visible.
  const tablesRoot = snapshot?.tablesRoot;
  const scoreSource = table.latest.game.source;
  if (tablesRoot && scoreSource) {
    try {
      table.vpxFileHash = await invoke<string | null>("resolve_vpx_hash", { tablesRoot, scoreSource });
    } catch {
      table.vpxFileHash = null;
    }
  }
  if (table.vpxFileHash) {
    const match = await fetchVPinPlay<{ vpsId?: string | null; altvpsid?: string | null }>(
      `/tables/by-filehash/${encodeURIComponent(table.vpxFileHash)}`,
    );
    const vpsId = match?.altvpsid || match?.vpsId;
    if (vpsId) return mediaForVpsId(vpsId);
  }
  return null;
}

function pickDominantVpsItem(items: VPinPlayItem[]): VPinPlayItem | null {
  const counts = new Map<string, number>();
  for (const item of items) {
    if (item.vpsId) counts.set(item.vpsId, (counts.get(item.vpsId) ?? 0) + 1);
  }
  const winner = [...counts.entries()].sort((left, right) => right[1] - left[1])[0]?.[0];
  return winner ? items.find((item) => item.vpsId === winner) ?? null : null;
}

async function mediaForVpsId(vpsId: string): Promise<TableMedia> {
  const response = await fetchVPinPlay<{ vpsdb?: { name?: string; manufacturer?: string; year?: string | number } }>(
    `/vpsdb/${encodeURIComponent(vpsId)}`,
  );
  return mediaFromItem({ vpsId, ...(response?.vpsdb ?? {}) }, vpsId);
}

function mediaFromItem(item: VPinPlayItem, vpsId: string): TableMedia {
  const metadata = item.vpsdb ?? item;
  return {
    vpsId,
    name: metadata.name || item.name || "Table",
    manufacturer: String(metadata.manufacturer || item.manufacturer || ""),
    year: String(metadata.year || item.year || ""),
    wheelUrl: `${VPINMEDIA_BASE}/${encodeURIComponent(vpsId)}/wheel.png`,
    resolvedAt: Date.now(),
  };
}

function pickBestVPinPlayItem(items: VPinPlayItem[], table: TableHistory): VPinPlayItem | null {
  const targets = [table.name, table.vpxFileName?.replace(/\.vpx$/i, "")]
    .filter((value): value is string => Boolean(value))
    .map(normalizeTableIdentity);
  let best: VPinPlayItem | null = null;
  let bestScore = 0;
  for (const item of items) {
    const name = normalizeTableIdentity(item.vpsdb?.name || item.name || "");
    const filename = normalizeTableIdentity((item.filename || "").replace(/\.vpx$/i, ""));
    const score = Math.max(...targets.map((target) => identityScore(target, name, filename)));
    if (score > bestScore) {
      best = item;
      bestScore = score;
    }
  }
  return bestScore >= 35 ? best : null;
}

function identityScore(target: string, name: string, filename: string): number {
  if (!target || !name) return 0;
  if (filename && target === filename) return 120;
  if (target === name) return 100;
  if (target.startsWith(name) || name.startsWith(target)) return 75;
  if (target.includes(name) || name.includes(target)) return 55;
  const targetTokens = new Set(target.split(" ").filter((token) => token.length > 2));
  const nameTokens = new Set(name.split(" ").filter((token) => token.length > 2));
  const shared = [...targetTokens].filter((token) => nameTokens.has(token)).length;
  return targetTokens.size ? (shared / targetTokens.size) * 50 : 0;
}

function normalizeTableIdentity(value: string): string {
  return value.toLowerCase().replace(/[^a-z0-9]+/g, " ").trim();
}

function mediaCacheKey(table: TableHistory): string {
  return table.vpxFileHash || normalizeTableIdentity(`${table.name}|${table.vpxFileName ?? ""}|${table.rom}`);
}

function readMediaCache(): Record<string, TableMedia> {
  try {
    return JSON.parse(localStorage.getItem(MEDIA_CACHE_KEY) ?? "{}") as Record<string, TableMedia>;
  } catch {
    return {};
  }
}

function writeMediaCache(cache: Record<string, TableMedia>): void {
  try {
    localStorage.setItem(MEDIA_CACHE_KEY, JSON.stringify(cache));
  } catch {
    // Media enrichment is optional; storage limits must not break local history.
  }
}

async function fetchVPinPlay<T>(path: string): Promise<T | null> {
  const controller = new AbortController();
  const timeout = window.setTimeout(() => controller.abort(), 7000);
  try {
    const response = await fetch(`${VPINPLAY_API_BASE}${path}`, { signal: controller.signal });
    if (!response.ok) return null;
    return await response.json() as T;
  } catch {
    return null;
  } finally {
    window.clearTimeout(timeout);
  }
}

function renderTableDetail(table: TableHistory): string {
  const targets = machineBoardTargets(table);
  const nextTarget = [...targets].sort((left, right) => left.score - right.score).find((target) => target.score > table.latest.score);
  const comparisonTarget = nextTarget ?? [...targets].sort((left, right) => right.score - left.score)[0];
  const latestPct = comparisonTarget ? Math.round((table.latest.score / comparisonTarget.score) * 100) : 100;
  const progressText = nextTarget ? `${latestPct}% of ${nextTarget.label.toLowerCase()}` : "Top machine score beaten";
  return `<section class="detail-hero">
      <div class="detail-identity">${renderWheel(table, "detail-wheel")}<div class="detail-title"><p class="eyebrow">${esc(detailEyebrow(table))}</p><h1>${esc(displayName(table))}</h1>${tableMedia.has(table.rom) ? `<span class="vps-id">VPS / ${esc(tableMedia.get(table.rom)?.vpsId)}</span>` : ""}</div></div>
      <div class="record-summary"><small>Latest game</small><strong>${number(table.latest.score)}</strong><span>${esc(progressText)}</span></div>
    </section>
    <section class="stats detail-stats" aria-label="Table summary">
      ${stat("Personal best", number(table.best), date([...table.entries].sort((left, right) => right.score - left.score)[0].date))}
      ${stat("Average score", number(table.average), `${table.entries.length} recorded scores`)}
      ${stat("Time played", duration(table.totalTime), `${table.games.length} completed games`)}
    </section>
    <section class="panel chart-panel">
      <div class="panel-heading"><div><p class="eyebrow">Personal scores</p><h2>Score journey</h2></div><span>${table.entries.length} recorded ${table.entries.length === 1 ? "score" : "scores"}</span></div>
      <div class="progress-layout">${renderProgressChart(table)}${renderChartLeaderboard(targets)}</div>
    </section>
    <section class="panel history-panel full-history"><div class="panel-heading"><div><p class="eyebrow">Every result</p><h2>Score history</h2></div><span>${table.entries.length} scores</span></div>${renderScoreHistory(table.entries)}</section>`;
}

function machineBoardTargets(table: TableHistory): ChartTarget[] {
  if (nvram?.rom === table.rom) {
    const targets = nvram.highScores
      .filter((entry) => !entry.label.toLowerCase().includes("buy-in") && !entry.shortLabel?.toLowerCase().startsWith("bi"))
      .filter((entry) => entry.score > 0)
      .map((entry, index) => ({
        score: entry.score,
        label: entry.shortLabel || entry.label,
        name: entry.label,
        initials: entry.initials,
        rank: index + 1,
      }));
    if (targets.length) return targets;
  }
  return [{ score: table.best, label: "Personal record", name: "Personal record", initials: "", rank: 0 }];
}

function renderProgressChart(table: TableHistory): string {
  const width = 1000, height = 320, left = 148, right = 32, top = 38, bottom = 52;
  const chartWidth = width - left - right, chartHeight = height - top - bottom;
  const scores = table.entries.map((entry) => entry.score);
  const scoreMin = Math.min(...scores);
  const scoreMax = Math.max(...scores);
  const spread = scoreMax - scoreMin;
  const padding = spread > 0
    ? Math.max(spread * .18, scoreMax * .06)
    : Math.max(scoreMax * .25, 1);
  const paddedMin = Math.max(0, scoreMin - padding);
  const paddedMax = Math.max(paddedMin + 1, scoreMax + padding);
  const roughStep = (paddedMax - paddedMin) / 3;
  const magnitude = 10 ** Math.floor(Math.log10(roughStep));
  const residual = roughStep / magnitude;
  const step = (residual <= 1.5 ? 1 : residual <= 3 ? 2 : residual <= 7 ? 5 : 10) * magnitude;
  const axisMin = Math.max(0, Math.floor(paddedMin / step) * step);
  const axisMax = Math.max(axisMin + step, Math.ceil(paddedMax / step) * step);
  const x = (index: number) => left + (table.entries.length === 1 ? chartWidth / 2 : (index / (table.entries.length - 1)) * chartWidth);
  const y = (score: number) => top + chartHeight - ((score - axisMin) / (axisMax - axisMin)) * chartHeight;
  const points = table.entries.map((entry, index) => `${x(index).toFixed(1)},${y(entry.score).toFixed(1)}`).join(" ");
  const tickValues: number[] = [];
  for (let value = axisMin; value <= axisMax + step / 2; value += step) tickValues.push(Math.round(value));
  const labels = tickValues.map((value) => `<g><line x1="${left}" y1="${y(value)}" x2="${width - right}" y2="${y(value)}"></line><text x="${left - 14}" y="${y(value) + 4}" text-anchor="end">${number(value)}</text></g>`).join("");
  const dots = table.entries.map((entry, index) => {
    const latest = index === table.entries.length - 1 ? " latest-score-point" : "";
    return `<circle class="score-point${latest}" cx="${x(index)}" cy="${y(entry.score)}" r="${latest ? 7 : 6}" tabindex="0" data-score="${entry.score}" data-date="${esc(date(entry.date))}"><title>${date(entry.date)} — ${number(entry.score)}</title></circle>`;
  }).join("");
  const firstDate = table.entries[0]?.date ?? "", lastDate = table.entries.at(-1)?.date ?? "";
  const bestLine = table.entries.length > 1 && table.latest.score !== table.best
    ? `<g class="personal-best"><line class="personal-best-line" x1="${left}" y1="${y(table.best)}" x2="${width - right}" y2="${y(table.best)}"></line>
      <text class="personal-best-label" x="${width - right}" y="${Math.max(15, y(table.best) - 8)}" text-anchor="end">Personal best · ${number(table.best)}</text></g>`
    : "";
  const dateIndexes = table.entries.length <= 6
    ? table.entries.map((_, index) => index)
    : [0, Math.floor((table.entries.length - 1) / 2), table.entries.length - 1];
  const dateLabels = dateIndexes.map((index) => `<text class="axis-date" x="${x(index)}" y="${height - 15}" text-anchor="${index === 0 ? "start" : index === table.entries.length - 1 ? "end" : "middle"}">${date(table.entries[index].date, false)}</text>`).join("");
  const latestY = y(table.latest.score);
  const latestLabelY = latestY < top + 20 ? latestY + 24 : latestY - 13;
  const line = table.entries.length > 1
    ? `<polyline class="score-line" points="${points}" vector-effect="non-scaling-stroke"></polyline>`
    : "";
  return `<div class="chart-wrap"><svg class="progress-chart" viewBox="0 0 ${width} ${height}" role="img" aria-labelledby="chart-title chart-desc">
    <title id="chart-title">Personal score progression for ${esc(table.name)}</title><desc id="chart-desc">${table.entries.length} ${table.entries.length === 1 ? "score" : "scores"} from ${date(firstDate, false)} to ${date(lastDate, false)}. Latest score: ${number(table.latest.score)}. Personal best: ${number(table.best)}.</desc>
    <g class="chart-grid">${labels}</g>${bestLine}
    ${line}<g class="score-dots">${dots}</g>
    <text class="latest-score-label" x="${x(table.entries.length - 1) - 12}" y="${latestLabelY}" text-anchor="end">${number(table.latest.score)}</text>
    ${dateLabels}
  </svg></div>`;
}

function renderChartLeaderboard(targets: ChartTarget[]): string {
  const machineTargets = targets.filter((target) => target.rank > 0);
  if (!machineTargets.length) {
    const status = nvramBusy ? "Reading machine scores…" : nvramError ? "Machine scores are unavailable for this table." : "Machine high scores appear here when mapped NVRAM is available.";
    return `<aside class="chart-leaderboard chart-leaderboard-empty"><p class="eyebrow">Leaderboard</p><strong>Personal record</strong><span>${esc(status)}</span></aside>`;
  }
  return `<aside class="chart-leaderboard" aria-label="Machine leaderboard">
    <div class="leaderboard-heading"><div><p class="eyebrow">Machine board</p><h3>High scores</h3></div><span>From NVRAM</span></div>
    <ol>${machineTargets.map((target) => `<li class="leaderboard-rank-${target.rank}">
      <span class="trophy-cell">${target.rank <= 3 ? trophyIcon(target.rank) : `<span class="rank-number">${target.rank}</span>`}</span>
      <span class="leaderboard-player"><strong>${esc(target.initials || "—")}</strong><small>${esc(target.name)}</small></span>
      <strong class="leaderboard-score">${number(target.score)}</strong>
    </li>`).join("")}</ol>
  </aside>`;
}

function trophyIcon(rank: number): string {
  return `<svg class="trophy trophy-${rank}" viewBox="0 0 24 24" role="img" aria-label="${rank === 1 ? "Gold" : rank === 2 ? "Silver" : "Bronze"} trophy">
    <path d="M7 2h10v3h3v4c0 3-1.8 5-5 5h-.2c-.7 1-1.7 1.7-2.8 2v2h4v2H8v-2h4v-2c-1.1-.3-2.1-1-2.8-2H9c-3.2 0-5-2-5-5V5h3V2Zm0 5H6v2c0 1.4.7 2.4 2.1 2.8A9.8 9.8 0 0 1 7 7Zm10 0a9.8 9.8 0 0 1-1.1 4.8C17.3 11.4 18 10.4 18 9V7h-1Z"></path>
  </svg>`;
}

function renderScoreHistory(entries: ScoreEntry[]): string {
  return `<div class="score-history">${[...entries].reverse().map((entry) => `<div class="score-row"><span><strong>${number(entry.score)}</strong><small>${date(entry.date)}</small></span><span class="score-row-meta">${entry.game.signed ? `<span class="signed-label" title="Verified ScoreTracker signature">signed</span>` : ""}<span>${duration(entry.duration)}</span></span></div>`).join("")}</div>`;
}

function stat(label: string, value: string, context: string): string {
  return `<article><span>${esc(label)}</span><strong>${esc(value)}</strong><small>${esc(context)}</small></article>`;
}

function renderSetup(root: string): string {
  return `<section class="hero setup-hero"><p class="eyebrow">Cabinet setup</p><h1>Connect your local files.</h1><p>Choose your VPX tables folder. ScoreTracker includes the NVRAM maps needed for machine high scores.</p></section>
    <section class="panel setup">
      <div class="folder-row"><span class="folder-number">01</span><div><h2>VPX tables</h2><p>Contains each table's <code>scores.json</code> and PinMAME NVRAM files.</p><code class="folder-path">${esc(root || "Not selected")}</code></div><button id="choose-root" class="button ${root ? "secondary" : "primary"}" type="button">${root ? "Change" : "Choose folder"}</button></div>
      ${root ? `<div class="setup-ready"><span>Tables folder connected</span><button id="setup-done" class="button primary" type="button">View score history</button></div>` : `<div class="setup-hint">Select your tables folder to continue.</div>`}
    </section>`;
}

function renderFoldersModal(root: string): string {
  return `<div class="modal-backdrop" data-close-folders>
    <section class="folders-modal panel" role="dialog" aria-modal="true" aria-labelledby="folders-title">
      <div class="modal-heading"><div><p class="eyebrow">Cabinet setup</p><h2 id="folders-title">Folders</h2></div><button id="close-folders" class="modal-close" type="button" aria-label="Close folders">×</button></div>
      <div class="folder-row"><span class="folder-number">01</span><div><h2>VPX tables</h2><p>Contains each table's <code>scores.json</code> and PinMAME NVRAM files.</p><code class="folder-path">${esc(root)}</code></div><button id="choose-root" class="button secondary" type="button">Change</button></div>
    </section>
  </div>`;
}

function renderEmptyHistory(): string {
  if (busy) return `<section class="panel empty"><strong>Scanning your tables…</strong><span>This can take a moment for a large collection.</span></section>`;
  return `<section class="panel empty"><strong>No non-zero scores found</strong><span>Play a supported table with ScoreTracker enabled, then refresh.</span></section>`;
}

function sparklinePoints(values: number[], width: number, height: number, pad: number): string {
  if (!values.length) return "";
  const max = Math.max(...values), min = Math.min(...values), range = max - min || 1;
  return values.map((value, index) => {
    const x = values.length === 1 ? width / 2 : pad + (index / (values.length - 1)) * (width - pad * 2);
    const y = max === min ? height / 2 : height - pad - ((value - min) / range) * (height - pad * 2);
    return `${x.toFixed(1)},${y.toFixed(1)}`;
  }).join(" ");
}

function scorePointCircles(entries: ScoreEntry[], points: string): string {
  const coordinates = points.split(" ");
  return entries.map((entry, index) => {
    const [cx, cy] = coordinates[index]?.split(",") ?? ["0", "0"];
    return `<circle class="score-point" cx="${cx}" cy="${cy}" r="4.5" data-score="${entry.score}" data-date="${esc(date(entry.date))}"><title>${date(entry.date)} — ${number(entry.score)}</title></circle>`;
  }).join("");
}

function areaPath(points: string, height: number): string {
  if (!points) return "";
  const pairs = points.split(" ");
  return `M ${pairs[0]} L ${pairs.slice(1).join(" L ")} L ${pairs.at(-1)?.split(",")[0]},${height} L ${pairs[0].split(",")[0]},${height} Z`;
}

function wireEvents(): void {
  wireScorePointTooltips();
  document.querySelectorAll<HTMLImageElement>(".wheel-image").forEach((image) => {
    const artwork = image.closest<HTMLElement>(".wheel-art");
    const updateArtworkState = () => {
      const loaded = image.naturalWidth > 0;
      artwork?.classList.toggle("wheel-loaded", loaded);
      artwork?.classList.toggle("wheel-missing", !loaded);
      artwork?.closest(".table-card-art")?.classList.toggle("media-missing", !loaded);
      if (!loaded) image.remove();
    };
    image.addEventListener("load", updateArtworkState, { once: true });
    image.addEventListener("error", updateArtworkState, { once: true });
    if (image.complete) updateArtworkState();
  });
  document.querySelector("#choose-root")?.addEventListener("click", chooseRoot);
  document.querySelector("#show-folders")?.addEventListener("click", openFolders);
  document.querySelector("#close-folders")?.addEventListener("click", closeFolders);
  document.querySelector("[data-close-folders]")?.addEventListener("click", (event) => {
    if (event.target === event.currentTarget) closeFolders();
  });
  document.querySelector("#setup-done")?.addEventListener("click", goHome);
  document.querySelector("#refresh")?.addEventListener("click", scanConfiguredRoot);
  document.querySelector("#check-update")?.addEventListener("click", () => {
    if (availableUpdate) void installAvailableUpdate();
    else void checkForUpdate(true);
  });
  document.querySelector("#install-update")?.addEventListener("click", () => void installAvailableUpdate());
  document.querySelector("#home")?.addEventListener("click", goHome);
  document.querySelector("#back")?.addEventListener("click", goHome);
  document.querySelectorAll<HTMLElement>("[data-rom]").forEach((element) => element.addEventListener("click", () => {
    location.hash = `#/table/${encodeURIComponent(element.dataset.rom ?? "")}`;
  }));
}

function wireScorePointTooltips(): void {
  let tooltip = document.querySelector<HTMLElement>("#score-tooltip");
  if (!tooltip) {
    tooltip = document.createElement("div");
    tooltip.id = "score-tooltip";
    tooltip.className = "score-tooltip";
    tooltip.setAttribute("role", "status");
    document.body.append(tooltip);
  }
  tooltip.classList.remove("visible");
  const show = (point: SVGCircleElement, clientX?: number, clientY?: number) => {
    if (!tooltip) return;
    tooltip.innerHTML = `<strong>${number(Number(point.dataset.score ?? 0))}</strong><span>${esc(point.dataset.date ?? "")}</span>`;
    const bounds = point.getBoundingClientRect();
    tooltip.style.left = `${clientX ?? bounds.left + bounds.width / 2}px`;
    tooltip.style.top = `${clientY ?? bounds.top}px`;
    tooltip.classList.add("visible");
  };
  const hide = () => tooltip?.classList.remove("visible");
  document.querySelectorAll<SVGCircleElement>(".score-point").forEach((point) => {
    point.addEventListener("pointerenter", (event) => show(point, event.clientX, event.clientY));
    point.addEventListener("pointermove", (event) => show(point, event.clientX, event.clientY));
    point.addEventListener("pointerleave", hide);
    point.addEventListener("focus", () => show(point));
    point.addEventListener("blur", hide);
  });
}

function goHome(): void {
  foldersOpen = false;
  if (location.hash) location.hash = "#/";
  else render();
}

function openFolders(): void {
  foldersOpen = true;
  render();
  document.querySelector<HTMLButtonElement>("#close-folders")?.focus();
}

function closeFolders(): void {
  foldersOpen = false;
  render();
  document.querySelector<HTMLButtonElement>("#show-folders")?.focus();
}

async function ensureNvram(table: TableHistory, tablesRoot: string, mapsRoot: string): Promise<void> {
  if (!mapsRoot || !tablesRoot || nvramBusy || nvramRom === table.rom) return;
  nvramRom = table.rom;
  nvram = null;
  nvramError = "";
  nvramBusy = true;
  render();
  try {
    nvram = await invoke<NvramDocument | null>("load_nvram", {
      tablesRoot,
      mapsRoot,
      rom: table.rom,
      scoreSource: table.latest.game.source,
    });
  } catch (error) {
    nvramError = error instanceof Error ? error.message : String(error);
  } finally {
    nvramBusy = false;
    render();
  }
}

async function chooseRoot(): Promise<void> {
  const selected = await open({ directory: true, multiple: false, title: "Choose your VPX tables folder" });
  if (typeof selected !== "string") return;
  localStorage.setItem(TABLES_ROOT_KEY, selected);
  snapshot = null;
  fatalError = "";
  await scanConfiguredRoot();
}

async function scanConfiguredRoot(): Promise<void> {
  const tablesRoot = localStorage.getItem(TABLES_ROOT_KEY);
  if (!tablesRoot || busy) return;
  busy = true;
  fatalError = "";
  render();
  try {
    snapshot = await invoke<ScanSnapshot>("scan_scores", { tablesRoot });
  } catch (error) {
    fatalError = error instanceof Error ? error.message : String(error);
  } finally {
    busy = false;
    nvramRom = "";
    nvram = null;
    render();
  }
}

async function checkForUpdate(manual: boolean): Promise<void> {
  if (updateBusy) return;
  updateBusy = true;
  if (manual) updateStatus = "Checking GitHub Releases…";
  render();
  try {
    availableUpdate = await invoke<UpdateInfo | null>("check_for_update");
    localStorage.setItem(UPDATE_CHECK_KEY, String(Date.now()));
    updateStatus = availableUpdate
      ? ""
      : manual
        ? "VPX Scoretracker Viewer is up to date."
        : "";
  } catch (error) {
    if (manual) {
      updateStatus = `Update check failed: ${error instanceof Error ? error.message : String(error)}`;
    }
  } finally {
    updateBusy = false;
    render();
  }
}

async function installAvailableUpdate(): Promise<void> {
  if (!availableUpdate || updateBusy) return;

  updateBusy = true;
  updateStatus = `Downloading and verifying ScoreTracker ${availableUpdate.version}…`;
  render();
  try {
    await invoke("download_and_launch_update", { update: availableUpdate });
  } catch (error) {
    updateStatus = `Update failed: ${error instanceof Error ? error.message : String(error)}`;
    updateBusy = false;
    render();
  }
}

window.addEventListener("hashchange", render);
window.addEventListener("keydown", (event) => {
  if (event.key === "Escape" && foldersOpen) closeFolders();
});
async function initialize(): Promise<void> {
  // Adopt installer defaults. The bundled maps path is authoritative and replaces
  // any map folder saved by older companion versions.
  try {
    const seed = await invoke<{ tablesRoot: string | null; mapsRoot: string | null } | null>("read_seed_config");
    if (seed?.tablesRoot && !localStorage.getItem(TABLES_ROOT_KEY)) {
      localStorage.setItem(TABLES_ROOT_KEY, seed.tablesRoot);
    }
    if (seed?.mapsRoot) {
      localStorage.setItem(MAPS_ROOT_KEY, seed.mapsRoot);
    }
  } catch {
    // no seed available; the setup screen will ask for the tables folder
  }

  const selectedMapsRoot = localStorage.getItem(MAPS_ROOT_KEY);
  if (selectedMapsRoot) {
    try {
      const resolved = await invoke<string>("resolve_maps_root", { path: selectedMapsRoot });
      localStorage.setItem(MAPS_ROOT_KEY, resolved);
    } catch (error) {
      localStorage.removeItem(MAPS_ROOT_KEY);
      nvramError = `Bundled maps unavailable: ${error instanceof Error ? error.message : String(error)}`;
    }
  }

  render();
  if (localStorage.getItem(TABLES_ROOT_KEY)) void scanConfiguredRoot();

  const lastCheck = Number(localStorage.getItem(UPDATE_CHECK_KEY) ?? 0);
  if (!Number.isFinite(lastCheck) || Date.now() - lastCheck >= UPDATE_CHECK_INTERVAL) {
    void checkForUpdate(false);
  }
}

// Paint the application immediately. Installer defaults are loaded
// asynchronously and will update the view once IPC is ready.
render();
void initialize();
