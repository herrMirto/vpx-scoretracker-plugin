import { readFile, readdir, realpath, stat } from 'node:fs/promises';
import { basename, dirname, extname, join } from 'node:path';
import type { NvramDocument } from '../src/types.ts';
import { isWithin, resolveSource } from './scores.ts';
import { resolveMaps } from './host.ts';

interface Segment { address: number; size: number; fileBase: number; nibble: string }
interface Context { map: any; segments: Segment[]; little: boolean; charMap?: string }
function integer(value: unknown, fallback = 0): number {
  const number = value === undefined ? fallback : Number(value);
  if (!Number.isSafeInteger(number)) throw new Error('Invalid map address or length');
  return number;
}
async function json(path: string): Promise<any> { return JSON.parse(await readFile(path, 'utf8')); }
async function mapFile(root: string, name: string): Promise<string> {
  const path = await realpath(join(root, name));
  if (!isWithin(root, path)) throw new Error('Map file is outside the maps folder');
  return path;
}
export async function loadMap(maps: string, rom: string): Promise<Context> {
  const root = await realpath(maps);
  const index = await json(await mapFile(root, 'index.json'));
  if (typeof index[rom] !== 'string') throw new Error(`No NVRAM map is available for ${rom}`);
  const map = await json(await mapFile(root, index[rom]));
  if (!map._metadata || typeof map._metadata !== 'object') throw new Error('Map has no _metadata');
  const platform = map._metadata.platform ? await json(await mapFile(root, `platforms/${map._metadata.platform}.json`)) : {};
  let fileBase = 0;
  const segments: Segment[] = (platform.memory_layout ?? []).filter((region: any) => region.type === 'nvram').map((region: any) => {
    const size = integer(region.size);
    if (size < 0) throw new Error('Invalid NVRAM segment size');
    const segment = {address: integer(region.address), size, fileBase, nibble: region.nibble ?? 'both'};
    fileBase += size; return segment;
  });
  return {map, segments, little: platform.endian === 'little', charMap: map._metadata.char_map};
}
export function decode(value: any, context: Context, data: Buffer): number | string | null {
  if (!value || typeof value !== 'object') return null;
  const start = integer(value.start);
  const end = value.length !== undefined ? start + integer(value.length, 1) - 1 : integer(value.end, start);
  if (end - start > 1_048_576) throw new Error('NVRAM descriptor is too large');
  const addresses: number[] = Array.isArray(value.offsets) ? value.offsets.map((address: unknown) => integer(address)) : Array.from({length: Math.max(0, end - start + 1)}, (_, i) => start + i);
  let nibble = 'both';
  const raw: number[] = [];
  for (const address of addresses) {
    const segment = context.segments.find(s => address >= s.address && address < s.address + s.size);
    if (context.segments.length && !segment) return null;
    const offset = segment ? segment.fileBase + address - segment.address : address;
    if (offset < 0 || offset >= data.length) return null;
    const kind = value.nibble ?? segment?.nibble ?? 'both';
    if (!raw.length) nibble = kind;
    let byte = kind === 'low' ? data[offset] & 15 : kind === 'high' ? data[offset] >> 4 : data[offset];
    if (value.mask !== undefined) byte &= integer(value.mask);
    raw.push(byte);
  }
  if (value.encoding === 'ch') {
    const chars = context.charMap ? [...context.charMap] : null;
    let text = '';
    for (const byte of raw) {
      if (value.null === 'terminate' && byte === 0) break;
      const char = chars ? chars[byte] ?? '' : String.fromCharCode(byte);
      if (!/[\x00-\x1f\x7f-\x9f]/.test(char)) text += char;
    }
    return text.trim();
  }
  if (value.endian ? value.endian === 'little' : context.little) raw.reverse();
  let number = 0;
  for (const byte of raw) {
    if (value.encoding === 'bcd') {
      number = nibble !== 'both' ? number * 10 + (byte <= 9 ? byte : 0)
        : number * 100 + (byte >> 4 <= 9 ? byte >> 4 : 0) * 10 + ((byte & 15) <= 9 ? byte & 15 : 0);
    } else number = number * 256 + byte;
  }
  if (value.encoding === 'bool') number = Number(number !== 0);
  return Math.round(number * (value.scale ?? 1) + (value.offset ?? 0));
}
async function newestNvram(root: string, rom: string): Promise<string | null> {
  let best: {path: string; modified: number} | null = null;
  async function visit(directory: string, depth: number): Promise<void> {
    if (depth >= 6) return;
    for (const entry of await readdir(directory, {withFileTypes: true})) {
      const path = join(directory, entry.name);
      if (entry.isDirectory()) await visit(path, depth + 1);
      else if (entry.isFile() && extname(entry.name).toLowerCase() === '.nv' && basename(entry.name, extname(entry.name)).split('-')[0].toLowerCase() === rom.toLowerCase()) {
        const modified = (await stat(path)).mtimeMs;
        if (!best || modified > best.modified) best = {path, modified};
      }
    }
  }
  await visit(root, 0);
  return (best as {path: string; modified: number} | null)?.path ?? null;
}
export async function loadNvram(tables: string, source: string, rom: string, vpx: string, override = ''): Promise<NvramDocument | null> {
  if (!/^[a-z0-9_]+$/i.test(rom)) throw new Error('Invalid ROM identifier');
  const root = await realpath(tables);
  const path = await resolveSource(root, source);
  const context = await loadMap(await resolveMaps(root, source, vpx, override), rom);
  const nvram = await newestNvram(dirname(path), rom) ?? (dirname(path) !== root ? await newestNvram(root, rom) : null);
  if (!nvram) return null;
  const data = await readFile(nvram);
  const highScores = (context.map.high_scores ?? []).flatMap((entry: any) => {
    const initials = decode(entry.initials, context, data);
    const score = decode(entry.score, context, data);
    const text = typeof initials === 'string' ? initials : '';
    const points = typeof score === 'number' ? score : 0;
    return points <= 0 && !text ? [] : [{label: entry.label ?? 'High Score', shortLabel: entry.short_label ?? null, initials: text, score: points}];
  });
  return {rom, highScores};
}
