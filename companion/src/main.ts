import { backend, request, post, cancelScan, scanScores, type Configuration } from "./api";
import packageMetadata from "../package.json";
import { normalizeTableIdentity, selectVPinPlayItem, type LookupAttempt, type VPinPlayItem } from "./media-match.ts";
import "./styles.css";
import type { GameRecord, NvramDocument, ScanSnapshot } from "./types";

let desktopMode = false;
let readOnly = true;
let vpxRoot = "";
let setupBusy = false;
let setupStatus = "";
let nativeChoosing = false;
let scanSerial = 0;
const sessionPreferences = new Map<string, string>();
const preferences = {
  getItem(key: string): string | null { try { return localStorage.getItem(key) ?? sessionPreferences.get(key) ?? null; } catch { return sessionPreferences.get(key) ?? null; } },
  setItem(key: string, value: string): void { sessionPreferences.set(key, value); try { localStorage.setItem(key, value); } catch { /* session storage remains usable */ } },
  removeItem(key: string): void { sessionPreferences.delete(key); try { localStorage.removeItem(key); } catch { /* unavailable */ } },
};
function route(): string { return location.hash; }
function navigate(value: string): void { location.hash = value; }
const TABLES_ROOT_KEY = "scoretracker.tablesRoot";
const MEDIA_CACHE_KEY = "scoretracker.tableMedia.v2";
const VIEWER_VERSION = packageMetadata.version;
const VPINPLAY_API_BASE = "https://api.vpinplay.com:8888/api/v1";
const VPINMEDIA_BASE = "https://raw.githubusercontent.com/superhac/vpinmediadb/refs/heads/main";
const MEDIA_CACHE_MAX_AGE = 30 * 24 * 60 * 60 * 1000;
const app = document.querySelector<HTMLDivElement>("#app")!;

if (!app) throw new Error("Application root was not found");

interface ScoreEntry {
  score: number;
  player: number;
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

type DiagnosticLevel = "debug" | "info" | "warn" | "error";

function errorDetails(error: unknown): Record<string, unknown> {
  if (error instanceof Error) return { name: error.name, message: error.message, stack: error.stack ?? "" };
  return { message: String(error) };
}

function diagnostic(level: DiagnosticLevel, event: string, details: Record<string, unknown> = {}): void {
  const consoleMethod = level === "error" ? console.error : level === "warn" ? console.warn : console.info;
  consoleMethod(`[ScoreTracker] ${event}`, details);
  if (desktopMode) void post("/api/log", {level, event, details}).catch(() => {});
}

interface ChartTarget {
  score: number;
  label: string;
  name: string;
  initials: string;
  rank: number;
  place: number;
  champion: boolean;
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
let webConfigurable = false;
let webConfigLoaded = false;
let tableSearch = "";
let tableSort: "recent" | "name" | "games" = "recent";
let detailTab: "overview" | "history" = "overview";
let chartRange: "all" | "5" = "all";
let selectedScoreIndex = -1;
const loggedWheelResults = new Set<string>();

function number(value: number): string {
  return new Intl.NumberFormat().format(value);
}

function duration(seconds: number | null): string {
  if (seconds === null) return "—";
  const value = Math.max(0, Math.floor(seconds));
  return value >= 3600 ? `${Math.floor(value / 3600)}h ${Math.floor(value % 3600 / 60)}m` : `${Math.floor(value / 60)}m ${value % 60}s`;
}

function fullDate(value: string): string {
  return new Intl.DateTimeFormat(undefined, {year: "numeric", month: "short", day: "numeric"}).format(new Date(value));
}

function time(value: string): string {
  return new Intl.DateTimeFormat(undefined, {hour: "2-digit", minute: "2-digit"}).format(new Date(value));
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
    if (!game.scores.some((score) => score >= 0)) continue;
    const key = game.rom || game.table;
    const group = grouped.get(key) ?? { name: game.table || game.rom, games: [] };
    group.games.push(game);
    grouped.set(key, group);
  }

  return [...grouped.entries()].map(([rom, group]) => {
    group.games.sort((left, right) => left.date.localeCompare(right.date));
    const entries = group.games.flatMap((game) => game.scores
      .map((score, index) => ({ score, player: index + 1, date: game.date, duration: game.gameDuration, game }))
      .filter((entry) => entry.score >= 0));
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
  const match = route().match(/^#\/table\/(.+)$/);
  return match ? decodeURIComponent(match[1]) : null;
}

function render(): void {
  const activeSearch = document.activeElement instanceof HTMLInputElement && document.activeElement.id === "search" ? document.activeElement : null;
  const selection = activeSearch ? [activeSearch.selectionStart, activeSearch.selectionEnd] : null;
  const root = preferences.getItem(TABLES_ROOT_KEY) ?? "";
  const tableList = tables();
  const selected = selectedRom();
  const table = selected ? tableList.find((candidate) => candidate.rom === selected) : null;
  const configured = Boolean(root);

  app.innerHTML = `
    ${renderTopbar(root, Boolean(table))}
    <main class="shell">
      ${renderNotices()}
      ${table && configured ? renderTableDetail(table) : configured ? renderOverview(tableList) : renderSetup(root)}
      ${renderFooter()}
    </main>
    ${foldersOpen && (configured || webConfigurable) ? renderFoldersModal(root) : ""}`;

  wireEvents();
  if (selection) {
    const input = document.querySelector<HTMLInputElement>("#search");
    input?.focus();
    input?.setSelectionRange(selection[0], selection[1]);
  }
  if (!busy && configured && tableList.length) void ensureTableMedia(tableList);
  if (table && configured) void ensureNvram(table, root);
}

function renderTopbar(root: string, inDetail: boolean): string {
  return `<header class="topbar">
    <button class="brand brand-button" id="home" type="button" aria-label="ScoreTracker home">
      <span class="brand-stripe" aria-hidden="true"></span><span class="brand-copy"><span class="brand-title"><strong>VPX ScoreTracker Viewer</strong><span class="viewer-version">v${esc(VIEWER_VERSION)}</span></span><small>Local scores</small></span>
    </button>
    <nav class="actions" aria-label="Application actions">
      ${inDetail ? `<button id="back" class="button secondary" type="button">← All tables</button>` : ""}
      ${webConfigurable ? `<button id="show-folders" class="button secondary" type="button">Folders</button>` : ""}
      ${root ? `<button id="refresh" class="button primary" type="button" ${busy ? "disabled" : ""}>${busy ? "Scanning…" : "Refresh scores"}</button>` : ""}
      ${desktopMode ? `<button id="quit-native" class="button danger" type="button">Quit ScoreTracker</button>` : ""}
    </nav>
  </header>`;
}

function scanStatusText(): string {
  return `Reading score history… ${number(snapshot?.sourcesScanned ?? 0)} files read · ${number(snapshot?.games.length ?? 0)} games found`;
}

function renderNotices(): string {
  const notices: string[] = [];
  if (busy) notices.push(`<div id="scan-status" class="notice scan-status" role="status">${scanStatusText()}</div>`);
  if (fatalError) notices.push(`<div class="notice error" role="alert">${esc(fatalError)}</div>`);
  if (snapshot?.warnings.length) {
    notices.push(`<details class="notice warning"><summary>${snapshot.warnings.length} source warning(s)</summary><ul>${snapshot.warnings.map((warning) => `<li><strong>${esc(warning.source)}</strong>: ${esc(warning.message)}</li>`).join("")}</ul></details>`);
  }
  return notices.join("");
}


function renderFooter(): string {
  return '<footer class="footer"><span>Scores stored locally</span></footer>';
}

function visibleTables(tableList: TableHistory[]): TableHistory[] {
  const query = tableSearch.trim().toLocaleLowerCase();
  const filtered = tableList.filter(table => `${displayName(table)} ${table.name} ${table.rom} ${tableMedia.get(table.rom)?.manufacturer ?? ""}`.toLocaleLowerCase().includes(query));
  return filtered.sort(tableSort === "name"
    ? (a, b) => displayName(a).localeCompare(displayName(b))
    : tableSort === "games" ? (a, b) => b.games.length - a.games.length
      : (a, b) => b.latest.date.localeCompare(a.latest.date));
}

function renderLibrary(tableList: TableHistory[]): string {
  const visible = visibleTables(tableList);
  return visible.length ? visible.map(renderTableCard).join("") : '<div class="empty">No tables match your search.</div>';
}

function renderOverview(tableList: TableHistory[]): string {
  const games = snapshot?.games ?? [];
  const totalTime = games.reduce((sum, game) => sum + (game.gameDuration ?? 0), 0);
  const recent = tableList[0];
  const best = recent?.entries.find(entry => entry.score === recent.best);
  return `<div class="heading"><div><div class="overline">Local scores</div><h1>Tables</h1></div>
    <div class="heading-meta"><div><strong>${number(tableList.length)}</strong><small>Played tables</small></div><div><strong>${number(games.length)}</strong><small>Completed games</small></div><div><strong>${duration(totalTime)}</strong><small>Time played</small></div></div></div>
    ${recent && best ? `<section class="highlights" aria-label="Collection highlights">
      <article class="feature"><div class="feature-art"><div class="orbit"></div>${renderWheel(recent, "feature-wheel")}</div>
        <div class="feature-copy"><div class="overline">Last played · ${esc(date(recent.latest.date, false))}</div><h2>${esc(displayName(recent))}</h2><p>${renderMediaMeta(recent)}${recent.games.length} completed games</p><a class="feature-link" href="#/table/${encodeURIComponent(recent.rom)}">View details <b aria-hidden="true">↗</b></a></div></article>
      <article class="session-card"><div class="session-title"><div class="overline">Personal best</div><div class="session-indicator">Local history</div></div><div class="session-score">${number(recent.best)}</div><div class="session-name">${esc(displayName(recent))} <span class="pill">Player ${best.player}</span></div><div class="session-bottom"><div><small>Latest score</small><strong>${number(recent.latest.score)}</strong></div><div><small>Recorded on</small><strong>${esc(fullDate(best.date))}</strong></div></div></article>
    </section>
    <div class="section-heading"><h2>The collection <span class="count" id="result-count">${visibleTables(tableList).length} tables</span></h2><div class="library-tools"><label class="search-box"><span class="search-icon" aria-hidden="true">⌕</span><input id="search" type="search" placeholder="Find a table or ROM…" aria-label="Find a table or ROM" value="${esc(tableSearch)}"></label><select class="sort" id="sort" aria-label="Sort tables"><option value="recent" ${tableSort === "recent" ? "selected" : ""}>Recently played</option><option value="name" ${tableSort === "name" ? "selected" : ""}>Name · A to Z</option><option value="games" ${tableSort === "games" ? "selected" : ""}>Most played</option></select></div></div>
    <section class="library" id="library" aria-label="Played tables">${renderLibrary(tableList)}</section>` : renderEmptyHistory()}`;
}

function renderTableCard(table: TableHistory): string {
  const recent = table.entries.slice(-8);
  const points = sparklinePoints(recent.map(entry => entry.score), 90, 22, 3);
  const coordinate = points.split(" ")[0]?.split(",") ?? ["45", "11"];
  return `<a class="table-tile" href="#/table/${encodeURIComponent(table.rom)}" aria-label="View ${esc(displayName(table))} details">
    ${renderWheel(table, "thumb")}<div class="table-copy"><h3>${esc(displayName(table))}</h3>${renderMediaMeta(table)}<div class="tile-meta"><span>${table.games.length} ${table.games.length === 1 ? "game" : "games"}</span><span class="meta-dot" aria-hidden="true"></span><span>Last played ${esc(date(table.latest.date, false))}</span></div></div>
    <div class="tile-best"><small>Personal best</small><strong>${number(table.best)}</strong><svg class="tile-spark" viewBox="0 0 90 22" role="img" aria-label="${recent.length} recent recorded scores">${recent.length > 1 ? `<polyline points="${points}" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"/>` : `<circle cx="${coordinate[0]}" cy="${coordinate[1]}" r="2.7" fill="currentColor"/>`}</svg></div></a>`;
}

function renderWheel(table: TableHistory, className: string): string {
  const media = tableMedia.get(table.rom);
  return `<div class="wheel-art ${className}" data-wheel-rom="${esc(table.rom)}">
    <span class="wheel-fallback">${esc(displayName(table))}</span>
    ${media ? `<img class="wheel-image" src="${esc(media.wheelUrl)}" alt="${esc(media.name)} wheel artwork" loading="${className === "thumb" ? "lazy" : "eager"}">` : ""}
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

async function ensureTableMedia(tableList: TableHistory[]): Promise<void> {
  const generation = `${snapshot?.generatedAt ?? ""}|${tableList.map(mediaCacheKey).join(";")}`;
  if (mediaGeneration === generation) return;
  mediaGeneration = generation;

  const cache = readMediaCache();
  const pending: TableHistory[] = [];
  let cacheHits = 0;
  for (const table of tableList) {
    const cached = cache[mediaCacheKey(table)];
    if (cached && Date.now() - cached.resolvedAt < MEDIA_CACHE_MAX_AGE) {
      tableMedia.set(table.rom, cached);
      cacheHits += 1;
      diagnostic("debug", "media.cache_hit", {
        table: table.name, rom: table.rom, vpsId: cached.vpsId, wheelUrl: cached.wheelUrl,
      });
    } else {
      pending.push(table);
    }
  }
  diagnostic("info", "media.scan_started", {
    tables: tableList.length, cacheHits, pending: pending.length, online: navigator.onLine,
  });

  for (let index = 0; index < pending.length; index += 4) {
    const chunk = pending.slice(index, index + 4);
    const resolved = await Promise.all(chunk.map(async (table) => ({ table, media: await resolveTableMedia(table) })));
    for (const { table, media } of resolved) {
      if (!media) {
        diagnostic("warn", "media.table_unresolved", {
          table: table.name, rom: table.rom, vpxFileName: table.vpxFileName,
        });
        continue;
      }
      tableMedia.set(table.rom, media);
      cache[mediaCacheKey(table)] = media;
      diagnostic("info", "media.table_resolved", {
        table: table.name, rom: table.rom, vpsId: media.vpsId, wheelUrl: media.wheelUrl,
      });
    }
  }

  writeMediaCache(cache);
  if (mediaGeneration === generation) render();
}

async function resolveTableMedia(table: TableHistory): Promise<TableMedia | null> {
  diagnostic("debug", "media.resolve_started", {
    table: table.name,
    rom: table.rom,
    vpxFileName: table.vpxFileName,
    scoreSource: table.latest.game.source,
  });
  // The initial scan intentionally does not hash full VPX files. Resolve the
  // exact PinMAME ROM before consulting user-controlled table/directory names.
  const selection = await selectVPinPlayItem(table, {
    byRom: async (rom) => {
      const response = await fetchVPinPlay<{ items?: VPinPlayItem[] }>(
        `/tables/by-rom/${encodeURIComponent(rom)}?limit=100`,
        `ROM search for ${rom}`,
      );
      return response?.items ?? [];
    },
    byName: async (name) => {
      const response = await fetchVPinPlay<{ items?: VPinPlayItem[] }>(
        `/vpsdb/search?q=${encodeURIComponent(name)}&limit=30`,
        `name search for ${name}`,
      );
      return response?.items ?? [];
    },
  }, (attempt: LookupAttempt) => {
    diagnostic("debug", attempt.source === "rom" ? "media.rom_search" : "media.name_search", {
      table: table.name,
      rom: table.rom,
      candidates: attempt.items.length,
      selectedVpsId: attempt.selected?.vpsId ?? null,
      selectedName: attempt.selected?.vpsdb?.name || attempt.selected?.name || null,
    });
  });
  if (selection?.item.vpsId) return mediaFromItem(selection.item, selection.item.vpsId);

  // Exact file hashing is an expensive last resort. It runs in a blocking worker
  // in the desktop backend, after local scores are already visible.
  const tablesRoot = snapshot?.tablesRoot;
  const scoreSource = table.latest.game.source;
  if (tablesRoot && scoreSource) {
    try {
      table.vpxFileHash = await backend<string | null>("resolve_vpx_hash", { tablesRoot, scoreSource });
      diagnostic("debug", "media.vpx_hash", {
        table: table.name, scoreSource, hash: table.vpxFileHash,
      });
    } catch (error) {
      table.vpxFileHash = null;
      diagnostic("error", "media.vpx_hash_failed", {
        table: table.name, scoreSource, ...errorDetails(error),
      });
    }
  }
  if (table.vpxFileHash) {
    const match = await fetchVPinPlay<{ vpsId?: string | null; altvpsid?: string | null }>(
      `/tables/by-filehash/${encodeURIComponent(table.vpxFileHash)}`,
      `file hash search for ${table.name}`,
    );
    const vpsId = match?.altvpsid || match?.vpsId;
    diagnostic("debug", "media.hash_search", {
      table: table.name, hash: table.vpxFileHash, vpsId: vpsId ?? null,
    });
    if (vpsId) return mediaForVpsId(vpsId);
  }
  return null;
}

async function mediaForVpsId(vpsId: string): Promise<TableMedia> {
  const response = await fetchVPinPlay<{ vpsdb?: { name?: string; manufacturer?: string; year?: string | number } }>(
    `/vpsdb/${encodeURIComponent(vpsId)}`,
    `VPS metadata for ${vpsId}`,
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

function mediaCacheKey(table: TableHistory): string {
  return table.vpxFileHash || normalizeTableIdentity(`${table.name}|${table.vpxFileName ?? ""}|${table.rom}`);
}

function readMediaCache(): Record<string, TableMedia> {
  try {
    return JSON.parse(preferences.getItem(MEDIA_CACHE_KEY) ?? "{}") as Record<string, TableMedia>;
  } catch (error) {
    diagnostic("warn", "media.cache_read_failed", errorDetails(error));
    return {};
  }
}

function writeMediaCache(cache: Record<string, TableMedia>): void {
  try {
    preferences.setItem(MEDIA_CACHE_KEY, JSON.stringify(cache));
  } catch (error) {
    // Media enrichment is optional; storage limits must not break local history.
    diagnostic("warn", "media.cache_write_failed", errorDetails(error));
  }
}

async function fetchVPinPlay<T>(path: string, purpose: string): Promise<T | null> {
  const controller = new AbortController();
  const timeout = window.setTimeout(() => controller.abort(), 7000);
  const url = `${VPINPLAY_API_BASE}${path}`;
  const startedAt = performance.now();
  try {
    const response = await fetch(url, { signal: controller.signal });
    const elapsedMs = Math.round(performance.now() - startedAt);
    if (!response.ok) {
      diagnostic("warn", "media.api_http_error", {
        purpose, url, status: response.status, statusText: response.statusText, elapsedMs,
      });
      return null;
    }
    try {
      const value = await response.json() as T;
      diagnostic("debug", "media.api_success", { purpose, url, status: response.status, elapsedMs });
      return value;
    } catch (error) {
      diagnostic("error", "media.api_json_failed", { purpose, url, elapsedMs, ...errorDetails(error) });
      return null;
    }
  } catch (error) {
    const elapsedMs = Math.round(performance.now() - startedAt);
    diagnostic("error", "media.api_request_failed", { purpose, url, elapsedMs, ...errorDetails(error) });
    return null;
  } finally {
    window.clearTimeout(timeout);
  }
}

function renderTableDetail(table: TableHistory): string {
  const targets = machineBoardTargets(table);
  const firstPlace = targets.find(target => target.rank > 0 && isFirstPlaceLabel(`${target.name} ${target.label}`));
  const gap = firstPlace ? Math.max(0, firstPlace.score - table.best) : null;
  const best = table.entries.find(entry => entry.score === table.best)!;
  return `<nav class="breadcrumb" aria-label="Breadcrumb"><a href="#/">Tables</a><span aria-hidden="true">/</span><span>${esc(displayName(table))}</span></nav>
    <div class="detail-layout"><aside class="identity">${renderWheel(table, "identity-art")}<h1>${esc(displayName(table))}</h1>${renderMediaMeta(table)}<p class="rom">${table.games.some(game => game.rom) ? esc(table.rom) : "Scripted table"}</p>
      <div class="identity-stats"><div><span>Completed games</span><strong>${table.games.length}</strong></div><div><span>Time played</span><strong>${duration(table.totalTime)}</strong></div><div><span>First recorded</span><strong>${esc(fullDate(table.entries[0].date))}</strong></div></div>
      <div class="best-block"><div class="overline">Personal best</div><strong>${number(table.best)}</strong><small>Player ${best.player} · ${esc(fullDate(best.date))}</small></div></aside>
    <section class="detail-main ${detailTab === "history" ? "history-only" : ""}" aria-label="Table performance"><div class="detail-top"><div class="tabs" role="tablist" aria-label="Table views"><button role="tab" aria-selected="${detailTab === "overview"}" class="tab ${detailTab === "overview" ? "active" : ""}" data-tab="overview">Overview</button><button role="tab" aria-selected="${detailTab === "history"}" class="tab ${detailTab === "history" ? "active" : ""}" data-tab="history">Game history <span class="count">${table.games.length}</span></button></div></div>
      <div class="detail-kpis"><div class="last"><small>Latest score</small><strong>${number(table.latest.score)}</strong><p>${esc(date(table.latest.date))} · Player ${table.latest.player}</p></div><div><small>Average score</small><strong>${number(table.average)}</strong><p>Across ${table.entries.length} player scores</p></div><div><small>Percentage remaining to first place</small><strong>${gap === null || !firstPlace ? "—" : gap === 0 ? "Reached" : `${Math.round(gap / firstPlace.score * 100)}%`}</strong><p>${gap === null ? "No mapped first-place score" : gap === 0 ? "Personal best ≥ machine #1" : `${number(gap)} points to go`}</p></div></div>
      <section class="chart-block"><div class="panel-title"><div><h2>Score history</h2><p>Scores by date · ${table.entries.length} recorded ${table.entries.length === 1 ? "score" : "scores"}</p></div><div class="segments" role="group" aria-label="Chart range"><button data-range="all" class="${chartRange === "all" ? "active" : ""}" aria-pressed="${chartRange === "all"}">All scores</button><button data-range="5" class="${chartRange === "5" ? "active" : ""}" aria-pressed="${chartRange === "5"}">Last 5</button></div></div><div class="chart-wrap" id="chart">${renderProgressChart(table)}</div></section>
      <div class="lower-grid"><section class="history-block"><div class="block-head"><h2>Game history</h2><span>${table.games.length} ${table.games.length === 1 ? "game" : "games"} · ${table.entries.length} ${table.entries.length === 1 ? "score" : "scores"}</span></div>${renderScoreHistory(table.entries)}</section>${renderChartLeaderboard(table, targets)}</div>
    </section></div>`;
}

function machineBoardTargets(table: TableHistory): ChartTarget[] {
  if (nvram?.rom === table.rom) {
    const entries = nvram.highScores
      .filter((entry) => !entry.label.toLowerCase().includes("buy-in") && !entry.shortLabel?.toLowerCase().startsWith("bi"))
      .filter((entry) => entry.score >= 0);
    const firstPlaceIndex = entries.findIndex((entry) => isFirstPlaceLabel(`${entry.label} ${entry.shortLabel ?? ""}`));
    let place = 0;
    const targets = entries
      .map((entry, index) => ({
        score: entry.score,
        label: entry.shortLabel || entry.label,
        name: entry.label,
        initials: entry.initials,
        rank: index + 1,
        champion: isChampionLabel(`${entry.label} ${entry.shortLabel ?? ""}`) || (firstPlaceIndex > 0 && index < firstPlaceIndex),
        place: 0,
      }))
      .map((target) => ({
        ...target,
        place: target.champion ? 0 : ++place,
      }));
    if (targets.length) return targets;
  }
  return [{ score: table.best, label: "Personal record", name: "Personal record", initials: "", rank: 0, place: 0, champion: false }];
}

function isChampionLabel(value: string): boolean {
  return /grand\s*champion|\bgc\b/i.test(value);
}

function isFirstPlaceLabel(value: string): boolean {
  return /(?:^|\W)(?:1st|first|#?1)(?:\W|$)/i.test(value);
}

function renderProgressChart(table: TableHistory): string {
  const start = chartRange === "5" ? Math.max(0, table.entries.length - 5) : 0;
  const entries = table.entries.slice(start);
  const width = 720, height = 222, left = 44, right = 20, top = 14, bottom = 30;
  const max = Math.max(table.best * 1.12, 1);
  const times = entries.map(entry => new Date(entry.date).getTime());
  const span = times[times.length - 1] - times[0];
  const x = (index: number) => span ? left + (times[index] - times[0]) / span * (width - left - right) : (left + width - right) / 2;
  const y = (score: number) => height - bottom - score / max * (height - top - bottom);
  const points = entries.map((entry, index) => `${x(index)},${y(entry.score)}`).join(" ");
  const labels = [0, 1, 2, 3].map(i => {
    const score = max * i / 3;
    const label = score >= 1e9 ? `${(score / 1e9).toFixed(1)}B` : score >= 1e6 ? `${(score / 1e6).toFixed(0)}M` : score >= 1000 ? `${(score / 1000).toFixed(0)}K` : String(Math.round(score));
    return `<line class="chart-grid" x1="${left}" y1="${y(score)}" x2="${width - right}" y2="${y(score)}"/><text class="chart-label" x="${left - 10}" y="${y(score) + 3}" text-anchor="end">${label}</text>`;
  }).join("");
  const sameDay = new Date(entries[0].date).toDateString() === new Date(entries[entries.length - 1].date).toDateString();
  let lastTick = -Infinity;
  const dots = entries.map((entry, index) => {
    const showTick = x(index) - lastTick >= 80;
    if (showTick) lastTick = x(index);
    const selected = selectedScoreIndex === index + start;
    return `<circle class="chart-point score-point ${selected ? "selected" : ""}" cx="${x(index)}" cy="${y(entry.score)}" r="${selected ? 5 : 3.5}" tabindex="0" data-score="${entry.score}" data-date="${esc(`${date(entry.date)} · Player ${entry.player}`)}"><title>${esc(date(entry.date))} · Player ${entry.player}: ${number(entry.score)} points</title></circle>${showTick ? `<text class="chart-label" x="${x(index)}" y="${height - 7}" text-anchor="middle">${esc(sameDay ? time(entry.date) : date(entry.date, false))}</text>` : ""}`;
  }).join("");
  const selected = table.entries[selectedScoreIndex];
  return `<svg class="score-chart" viewBox="0 0 ${width} ${height}" role="img" aria-label="${entries.length} recorded scores; highest score ${number(table.best)} points"><defs><linearGradient id="chart-fill" x1="0" y1="0" x2="0" y2="1"><stop offset="0%" stop-color="#171717" stop-opacity=".1"/><stop offset="100%" stop-color="#171717" stop-opacity="0"/></linearGradient></defs>${labels}${span ? `<polygon points="${x(0)},${height - bottom} ${points} ${x(entries.length - 1)},${height - bottom}" fill="url(#chart-fill)"/>` : ""}<polyline class="chart-line" points="${points}"/>${dots}</svg>
    <div class="chart-selection"><span>${selected ? `${esc(date(selected.date))} · Player ${selected.player} · ${number(selected.score)} points` : "Select a score in the history to highlight it."}</span><strong>Personal scores</strong></div>`;
}

function renderChartLeaderboard(table: TableHistory, targets: ChartTarget[]): string {
  const machine = targets.filter(target => target.rank > 0);
  const status = nvramBusy ? "Reading machine scores…" : "No machine high scores available.";
  return `<aside class="leaderboard" aria-label="Table high scores"><div class="block-head"><h2>Table high scores</h2><span>NVRAM</span></div>
    ${machine.length ? machine.map(target => `<div class="rank-row ${target.champion ? "champion" : ""}"><span class="rank-no">${esc(target.label)}</span><div class="rank-body"><div><span>${esc(target.name)}</span><span class="initials">${esc(target.initials)}</span></div><strong>${number(target.score)}</strong></div></div>`).join("") : `<div class="machine-empty">${esc(status)}${nvramError ? `<details><summary>Details</summary>${esc(nvramError)}</details>` : ""}</div>`}
    <div class="rank-row you"><span class="rank-no">↳</span><div class="rank-body"><div><span>Your personal best</span><span>YOU</span></div><strong>${number(table.best)}</strong></div></div><div class="rank-foot">Machine scores are a read-only reference.</div></aside>`;
}

function renderScoreHistory(entries: ScoreEntry[]): string {
  const best = Math.max(...entries.map(entry => entry.score));
  const bestIndex = entries.findIndex(entry => entry.score === best);
  return `<div class="history-scroll"><table class="history-table"><thead><tr><th>Played</th><th>Player</th><th class="score">Final score</th><th>Duration</th>${readOnly ? "" : '<th><span class="sr-only">Actions</span></th>'}</tr></thead><tbody>${entries.map((entry, index) => ({entry, index})).reverse().map(({entry, index}) => `<tr data-game="${index}" tabindex="0" aria-label="Select ${esc(date(entry.date))}, player ${entry.player}, ${number(entry.score)} points" class="${selectedScoreIndex === index ? "selected" : ""}"><td>${esc(fullDate(entry.date))}<small>${esc(time(entry.date))}</small></td><td>P${entry.player}${index === bestIndex ? ' <span class="pill">PB</span>' : ""}</td><td class="score">${number(entry.score)}</td><td>${duration(entry.duration)}</td>${readOnly ? "" : `<td><button class="score-remove" type="button" data-source="${esc(entry.game.source)}" data-index="${entry.game.sourceIndex}" data-score-id="${esc(entry.game.scoreId ?? "")}" aria-label="Remove this game from history">Remove</button></td>`}</tr>`).join("")}</tbody></table></div>`;
}

function renderSetup(root: string): string {
  return `<section class="hero setup-hero"><p class="eyebrow">Local score history</p><h1>Tables folder</h1><p>Choose your tables folder to explore your history and machine leaderboards.</p></section>
    <section class="panel setup-panel">${webConfigLoaded ? webConfigurable ? renderWebFolders(root) : "<p>The dashboard folders are configured by its server.</p>" : '<p>Connecting to your Viewer…</p><button id="retry-connection" class="button primary">Retry connection</button>'}</section>`;
}
function renderWebFolders(root: string): string {
  if (!desktopMode) return '<p>The dashboard folders are configured by its server.</p>';
  return `<div class="folder-row"><div><h2>Tables folder</h2><code class="folder-path">${esc(root || "Not selected")}</code><p>Your folder is remembered automatically.</p></div><button id="choose-native" class="button primary" type="button">${root ? "Change tables folder" : "Choose tables folder"}</button></div>
  <form id="setup-form" class="web-folders-form">
    <label for="vpx-path">Visual Pinball installation</label><div class="setup-path"><input id="vpx-path" name="vpxRoot" value="${esc(vpxRoot)}" placeholder="Select your Visual Pinball installation"><button type="button" id="choose-vpx" class="button secondary">Browse…</button></div>
    <button type="submit" class="button secondary" ${setupBusy || !root ? "disabled" : ""}>Save folders</button>
    <p>Install or update the plugin using its separate ZIP in the VPX plugins directory.</p>
    <p id="setup-status" role="status">${esc(setupStatus)}</p>
  </form>`;
}
function renderFoldersModal(root: string): string {
  return `<div class="modal-backdrop" data-close-folders><section class="folders-modal panel" role="dialog" aria-modal="true" aria-labelledby="folders-title"><div class="modal-heading"><h2 id="folders-title">Folders & setup</h2><button id="close-folders" class="modal-close" aria-label="Close folders">×</button></div>${renderWebFolders(root)}${desktopMode ? '<button id="show-log" class="button secondary">Show diagnostic log</button>' : ""}</section></div>`;
}

function renderEmptyHistory(): string {
  if (busy) return `<section class="panel empty"><strong>Scanning your tables…</strong><span>This can take a moment for a large collection.</span></section>`;
  return `<section class="panel empty"><strong>No completed games found</strong><span>Play a supported table with ScoreTracker enabled, then refresh.</span></section>`;
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

function wireArtwork(scope: ParentNode = document): void {
  scope.querySelectorAll<HTMLImageElement>(".wheel-image").forEach((image) => {
    const artwork = image.closest<HTMLElement>(".wheel-art");
    const updateArtworkState = () => {
      const loaded = image.naturalWidth > 0;
      const resultKey = `${loaded ? "loaded" : "failed"}|${image.src}`;
      if (!loggedWheelResults.has(resultKey)) {
        loggedWheelResults.add(resultKey);
        diagnostic(loaded ? "info" : "error", loaded ? "media.wheel_loaded" : "media.wheel_failed", {
          rom: artwork?.dataset.wheelRom ?? "",
          url: image.src,
          naturalWidth: image.naturalWidth,
          naturalHeight: image.naturalHeight,
          online: navigator.onLine,
        });
      }
      artwork?.classList.toggle("wheel-loaded", loaded);
      artwork?.classList.toggle("wheel-missing", !loaded);
      artwork?.closest(".table-card-art")?.classList.toggle("media-missing", !loaded);
      if (!loaded) image.remove();
    };
    image.addEventListener("load", updateArtworkState, { once: true });
    image.addEventListener("error", updateArtworkState, { once: true });
    if (image.complete) updateArtworkState();
  });
}

function updateLibrary(): void {
  const library = document.querySelector<HTMLElement>("#library");
  if (!library) return;
  const list = tables();
  library.innerHTML = renderLibrary(list);
  const count = document.querySelector("#result-count");
  if (count) count.textContent = `${visibleTables(list).length} tables`;
  wireArtwork(library);
}

function wireEvents(): void {
  document.querySelector<HTMLInputElement>("#search")?.addEventListener("input", event => {
    tableSearch = (event.target as HTMLInputElement).value;
    updateLibrary();
  });
  document.querySelector<HTMLSelectElement>("#sort")?.addEventListener("change", event => {
    const value = (event.target as HTMLSelectElement).value;
    if (value === "recent" || value === "name" || value === "games") tableSort = value;
    updateLibrary();
  });
  document.querySelectorAll<HTMLButtonElement>("[data-tab]").forEach(button => button.addEventListener("click", () => {
    detailTab = button.dataset.tab === "history" ? "history" : "overview";
    render();
  }));
  document.querySelectorAll<HTMLButtonElement>("[data-range]").forEach(button => button.addEventListener("click", () => {
    chartRange = button.dataset.range === "5" ? "5" : "all";
    render();
  }));
  document.querySelectorAll<HTMLElement>("[data-game]").forEach(row => {
    const select = () => {
      selectedScoreIndex = Number(row.dataset.game);
      const table = tables().find(table => table.rom === selectedRom());
      if (!table) return;
      if (chartRange === "5" && selectedScoreIndex < table.entries.length - 5) { chartRange = "all"; render(); return; }
      document.querySelectorAll<HTMLElement>("[data-game]").forEach(other => other.classList.toggle("selected", other === row));
      const chart = document.querySelector("#chart");
      if (chart) chart.innerHTML = renderProgressChart(table);
      wireScorePointTooltips();
    };
    row.addEventListener("click", select);
    row.addEventListener("keydown", event => { if (event.target === row && (event.key === "Enter" || event.key === " ")) { event.preventDefault(); select(); } });
  });
  wireScorePointTooltips();
  wireArtwork();
  document.querySelectorAll<HTMLButtonElement>(".score-remove").forEach((button) => {
    button.addEventListener("click", (event) => { event.stopPropagation(); void removeGame(button); });
  });
  document.querySelector("#choose-native")?.addEventListener("click", () => { void chooseNativeFolder(); });
  document.querySelector("#quit-native")?.addEventListener("click", () => { void quitNative(); });
  document.querySelector("#retry-connection")?.addEventListener("click", () => { void initialize(); });
  document.querySelector<HTMLFormElement>("#setup-form")?.addEventListener("submit", event => { event.preventDefault(); void saveSetup(); });
  document.querySelector("#choose-vpx")?.addEventListener("click", () => { void chooseSetupPath("vpx"); });
  document.querySelector("#show-log")?.addEventListener("click", () => { void post("/api/reveal-log").catch(error => { fatalError = String(error); render(); }); });
  document.querySelector("#show-folders")?.addEventListener("click", openFolders);
  document.querySelector("#close-folders")?.addEventListener("click", closeFolders);
  document.querySelector("[data-close-folders]")?.addEventListener("click", (event) => {
    if (event.target === event.currentTarget) closeFolders();
  });
  document.querySelector("#refresh")?.addEventListener("click", () => {
    void scanConfiguredRoot();
  });
  document.querySelector("#home")?.addEventListener("click", goHome);
  document.querySelector("#back")?.addEventListener("click", goHome);
  document.querySelectorAll<HTMLElement>("[data-rom]").forEach((element) => element.addEventListener("click", () => {
    navigate(`#/table/${encodeURIComponent(element.dataset.rom ?? "")}`);
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
  if (route()) navigate("#/");
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

async function ensureNvram(table: TableHistory, tablesRoot: string): Promise<void> {
  if (!tablesRoot || nvramBusy || nvramRom === table.rom) return;
  nvramRom = table.rom;
  if (!table.games.some(game => game.rom)) return;
  nvram = null;
  nvramError = "";
  nvramBusy = true;
  render();
  try {
    nvram = await backend<NvramDocument | null>("load_nvram", {
      tablesRoot,
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

function adoptConfig(config: Configuration): void {
  desktopMode = config.native; readOnly = config.readOnly;
  webConfigurable = config.configurable; webConfigLoaded = true; vpxRoot = config.vpxRoot;
  preferences.setItem(TABLES_ROOT_KEY, config.tablesRoot);
}
async function chooseSetupPath(kind: "vpx"): Promise<void> {
  try {
    const result = await post<{path:string} | null>(`/api/choose-folder?kind=${kind}`);
    if (result) { const input = document.querySelector<HTMLInputElement>("#vpx-path"); if (input) input.value = result.path; }
  } catch (error) { setupStatus = String(error); const status = document.querySelector("#setup-status"); if (status) status.textContent = setupStatus; }
}
async function saveSetup(): Promise<void> {
  if (setupBusy) return;
  const form = document.querySelector<HTMLFormElement>("#setup-form"); if (!form) return;
  const values = new FormData(form);
  const settings = {tablesRoot: preferences.getItem(TABLES_ROOT_KEY) ?? "", vpxRoot: String(values.get("vpxRoot") ?? ""), mapsRoot: ""};
  setupBusy = true; setupStatus = "Saving folders…";
  vpxRoot = settings.vpxRoot; render();
  try {
    adoptConfig(await post<Configuration>("/api/config", settings)); setupStatus = "Folders saved.";
    nvram = null; nvramRom = "";
  } catch (error) { setupStatus = error instanceof Error ? error.message : String(error); }
  finally { setupBusy = false; render(); }
}
async function chooseNativeFolder(): Promise<void> {
  if (nativeChoosing) return;
  nativeChoosing = true;
  const button = document.querySelector<HTMLButtonElement>("#choose-native");
  if (button) { button.disabled = true; button.textContent = "Choose in the folder window…"; }
  try {
    const config = await post<Configuration | null>("/api/choose-folder");
    if (!config) return;
    cancelScan(); scanSerial++; busy = false;
    preferences.setItem(TABLES_ROOT_KEY, config.tablesRoot);
    adoptConfig(config);
    snapshot = null; nvram = null; nvramRom = ""; fatalError = ""; foldersOpen = false;
    navigate("");
    await scanConfiguredRoot();
  } catch (error) { fatalError = error instanceof Error ? error.message : String(error); }
  finally { nativeChoosing = false; render(); }
}
async function quitNative(): Promise<void> {
  try {
    await post("/api/quit");
    app.innerHTML = '<main><section class="panel"><h1>ScoreTracker is closed.</h1><p>Open the application to view your scores again.</p></section></main>';
  } catch (error) { fatalError = String(error); render(); }
}

async function scanConfiguredRoot(): Promise<void> {
  const tablesRoot = preferences.getItem(TABLES_ROOT_KEY);
  if (!tablesRoot || busy) return;
  const serial = ++scanSerial;
  selectedScoreIndex = -1;
  const refreshingExistingHistory = Boolean(snapshot?.games.length);
  busy = true;
  fatalError = "";
  diagnostic("info", "scores.scan_started", { tablesRoot });
  render();
  try {
    const started = performance.now();
    const scanned = await scanScores(partial => {
      if (serial !== scanSerial) return;
      const previousGames = snapshot?.games.length ?? 0;
      snapshot = partial;
      if (foldersOpen) return;
      // Keep existing cards and their artwork steady while refreshing. On the
      // first load, redraw only when a batch adds scores, not on every tick.
      if (!refreshingExistingHistory && partial.games.length !== previousGames) render();
      else {
        const status = document.querySelector("#scan-status");
        if (status) status.textContent = scanStatusText();
      }
    });
    if (serial !== scanSerial) return;
    snapshot = scanned;
    diagnostic("info", "scores.scan_completed", {
      tablesRoot,
      games: snapshot.games.length,
      sourcesScanned: snapshot.sourcesScanned,
      elapsedMs: Math.round(performance.now() - started),
      vpxFilesFound: snapshot.vpxFilesFound,
      warnings: snapshot.warnings.length,
    });
  } catch (error) {
    if (serial !== scanSerial) return;
    fatalError = error instanceof Error ? error.message : String(error);
    diagnostic("error", "scores.scan_failed", { tablesRoot, ...errorDetails(error) });
  } finally {
    if (serial !== scanSerial) return;
    busy = false;
    nvramRom = "";
    nvram = null;
    render();
  }
}

async function removeGame(button: HTMLButtonElement): Promise<void> {
  if (busy) return;
  const tablesRoot = preferences.getItem(TABLES_ROOT_KEY);
  if (!tablesRoot) return;
  const scoreSource = button.dataset.source ?? "";
  const sourceIndex = Number(button.dataset.index ?? "0");
  const rawId = button.dataset.scoreId ?? "";
  const scoreId = rawId === "" ? null : rawId;
  if (!window.confirm("Remove this game from your history? It is permanently deleted from scores.json and cannot be undone.")) {
    return;
  }
  button.disabled = true;
  try {
    await backend("remove_game", { tablesRoot, scoreSource, sourceIndex, scoreId });
    await scanConfiguredRoot();
  } catch (error) {
    fatalError = error instanceof Error ? error.message : String(error);
    render();
  }
}

window.addEventListener("hashchange", () => { detailTab = "overview"; chartRange = "all"; selectedScoreIndex = -1; render(); window.scrollTo(0, 0); });
window.addEventListener("keydown", (event) => {
  if (event.key === "Escape" && foldersOpen) closeFolders();
});
async function initialize(): Promise<void> {
  try {
    adoptConfig(await request<Configuration>("/api/config"));
    fatalError = "";
    render();
    if (preferences.getItem(TABLES_ROOT_KEY)) void scanConfiguredRoot();

  } catch (error) { fatalError = `Cannot connect to the Viewer. ${String(error)}`; render(); }
}

// Paint immediately. Saved folder settings are loaded
// asynchronously and will update the view once the local API is ready.
render();
void initialize();
