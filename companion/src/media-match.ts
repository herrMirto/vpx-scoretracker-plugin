export interface VPinPlayItem {
  vpsId?: string;
  name?: string;
  manufacturer?: string;
  year?: string | number;
  filename?: string;
  filehash?: string;
  vpsdb?: { name?: string; manufacturer?: string; year?: string | number };
}

export interface TableIdentity {
  rom: string;
  name: string;
  vpxFileName: string | null;
}

export type LookupSource = "rom" | "name";

export interface LookupAttempt {
  source: LookupSource;
  items: VPinPlayItem[];
  selected: VPinPlayItem | null;
}

export interface VPinPlayLookup {
  byRom(rom: string): Promise<VPinPlayItem[]>;
  byName(name: string): Promise<VPinPlayItem[]>;
}

export interface VPinPlaySelection {
  source: LookupSource;
  item: VPinPlayItem;
}

/**
 * Resolve the strongest inexpensive table identity first. A PinMAME ROM lookup
 * is exact, while a table/directory name is user-controlled and may be as short
 * as "mb". Name search is therefore only a fallback.
 */
export async function selectVPinPlayItem(
  table: TableIdentity,
  lookup: VPinPlayLookup,
  observe?: (attempt: LookupAttempt) => void,
): Promise<VPinPlaySelection | null> {
  if (table.rom) {
    const items = await lookup.byRom(table.rom);
    const selected = pickBestVPinPlayItem(items, table) ?? pickDominantVpsItem(items);
    observe?.({ source: "rom", items, selected });
    if (selected?.vpsId) return { source: "rom", item: selected };
  }

  const query = table.name.trim();
  if (query) {
    const items = await lookup.byName(query);
    const selected = pickBestVPinPlayItem(items, table);
    observe?.({ source: "name", items, selected });
    if (selected?.vpsId) return { source: "name", item: selected };
  }

  return null;
}

export function pickDominantVpsItem(items: VPinPlayItem[]): VPinPlayItem | null {
  const counts = new Map<string, number>();
  for (const item of items) {
    if (item.vpsId) counts.set(item.vpsId, (counts.get(item.vpsId) ?? 0) + 1);
  }
  const winner = [...counts.entries()].sort((left, right) => right[1] - left[1])[0]?.[0];
  return winner ? items.find((item) => item.vpsId === winner) ?? null : null;
}

export function pickBestVPinPlayItem(items: VPinPlayItem[], table: TableIdentity): VPinPlayItem | null {
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

export function identityScore(target: string, name: string, filename: string): number {
  if (!target || !name) return 0;
  if (filename && target === filename) return 120;
  if (target === name) return 100;

  // Compare complete normalized words/phrases. Raw substring matching made
  // short directory names unsafe: "mb" incorrectly matched "catacomb".
  if (target.startsWith(`${name} `) || name.startsWith(`${target} `)) return 75;
  if (containsPhrase(target, name) || containsPhrase(name, target)) return 55;

  const targetTokens = new Set(target.split(" ").filter((token) => token.length > 2));
  const nameTokens = new Set(name.split(" ").filter((token) => token.length > 2));
  const shared = [...targetTokens].filter((token) => nameTokens.has(token)).length;
  return targetTokens.size ? (shared / targetTokens.size) * 50 : 0;
}

function containsPhrase(value: string, phrase: string): boolean {
  return ` ${value} `.includes(` ${phrase} `);
}

export function normalizeTableIdentity(value: string): string {
  return value.toLowerCase().replace(/[^a-z0-9]+/g, " ").trim();
}
