import { readdir, readFile, realpath, stat } from 'node:fs/promises';
import { createReadStream } from 'node:fs';
import { basename, dirname, extname, isAbsolute, join, relative } from 'node:path';
import { createHash } from 'node:crypto';
import type { GameRecord, ScanSnapshot } from '../src/types.ts';
import { parseExact, stringifyExact } from './json.ts';
import { atomicWrite, withHistoryLock } from './storage.ts';

export function isWithin(root: string, path: string): boolean {
  const name = relative(root, path);
  return name !== '..' && !name.startsWith(`..${process.platform === 'win32' ? '\\' : '/'}`) && !isAbsolute(name);
}
export async function resolveSource(tables: string, source: string): Promise<string> {
  const root = await realpath(tables);
  const path = await realpath(join(root, source));
  if (!isWithin(root, path) || basename(path) !== 'scores.json' || !(await stat(path)).isFile()) {
    throw new Error('Score source must be a scores.json file inside the tables folder');
  }
  return path;
}
function records(text: string, source: string): GameRecord[] {
  const document = parseExact(text);
  if (document?.schema_version !== 1n || !Array.isArray(document.games)) throw new Error('Unsupported scores schema version');
  const ids = new Set<string>();
  return document.games.map((game: any, sourceIndex: number) => {
    if (typeof game?.id !== 'string' || !/^[a-f0-9]{32}$/.test(game.id) || ids.has(game.id)) throw new Error('Invalid or duplicate game id');
    ids.add(game.id);
    if (typeof game.table !== 'string' || !game.table || /[/\\]/.test(game.table)) throw new Error('Invalid table filename');
    if (game.rom !== undefined && typeof game.rom !== 'string') throw new Error('Invalid ROM identifier');
    if (!Array.isArray(game.scores) || !game.scores.length || game.scores.length > 8 || game.scores.some((s: any, i: number) =>
      s?.player !== BigInt(i + 1) || typeof s.score !== 'bigint' || s.score < 0n || s.score > 9223372036854775807n)) throw new Error('Invalid player scores');
    if (typeof game.duration_ms !== 'bigint' || game.duration_ms < 0n || game.duration_ms > 9223372036854775807n ||
      typeof game.ended_at !== 'string' || !/^\d{4}-\d\d-\d\dT.*(?:Z|[+-]\d\d:\d\d)$/.test(game.ended_at) || !Number.isFinite(Date.parse(game.ended_at))) throw new Error('Invalid game date or duration');
    return {scoreId: game.id, date: game.ended_at, rom: game.rom ?? '', scores: game.scores.map((s: any) => Number(s.score)),
      gameDuration: Number(game.duration_ms / 1000n), gameState: null, table: basename(game.table, extname(game.table)),
      source, sourceIndex, vpxFileName: game.table, vpxFileHash: null};
  });
}

export async function scanScores(tables: string, progress?: (snapshot: ScanSnapshot) => Promise<void>, signal?: AbortSignal): Promise<ScanSnapshot> {
  const root = await realpath(tables);
  if (!(await stat(root)).isDirectory()) throw new Error('Tables folder does not exist or is not a directory');
  const snapshot: ScanSnapshot = {generatedAt: new Date().toISOString(), tablesRoot: root, sourcesScanned: 0, vpxFilesFound: 0, games: [], warnings: []};
  let last = 0;
  const update = async (force = false) => {
    signal?.throwIfAborted();
    if (progress && (force || Date.now() - last >= 500)) {
      last = Date.now(); sort(); await progress(snapshot);
    }
  };
  function sort() {
    snapshot.games.sort((a, b) => b.date.localeCompare(a.date) || a.source.localeCompare(b.source) || a.sourceIndex - b.sourceIndex);
    snapshot.warnings.sort((a, b) => a.source.localeCompare(b.source));
  }
  await update(true);
  let directories = [root];
  while (directories.length) {
    const next: string[] = [];
    // Parallelize directory I/O without unbounded open files or media traversal.
    for (let i = 0; i < directories.length; i += 8) {
      await Promise.all(directories.slice(i, i + 8).map(async directory => {
        signal?.throwIfAborted();
        try {
          const entries = await readdir(directory, {withFileTypes: true});
          const isTable = entries.some(entry => entry.isFile() && (entry.name === 'scores.json' || extname(entry.name).toLowerCase() === '.vpx'));
          for (const entry of entries) {
            signal?.throwIfAborted();
            const path = join(directory, entry.name);
            if (entry.isDirectory() && !isTable) next.push(path);
            if (!entry.isFile()) continue;
            if (extname(entry.name).toLowerCase() === '.vpx') snapshot.vpxFilesFound++;
            if (entry.name === 'scores.json') {
              snapshot.sourcesScanned++;
              try { snapshot.games.push(...records(await readFile(path, 'utf8'), relative(root, path))); }
              catch (error) { snapshot.warnings.push({source: relative(root, path), message: String(error)}); }
            }
          }
        } catch (error) {
          signal?.throwIfAborted();
          if ((error as NodeJS.ErrnoException).code !== 'ENOENT' || directory === root) snapshot.warnings.push({source: relative(root, directory), message: String(error)});
        }
      }));
      await update();
    }
    directories = next;
  }
  sort(); return snapshot;
}

export async function hashVpx(tables: string, source: string): Promise<string | null> {
  const directory = dirname(await resolveSource(tables, source));
  const candidates = (await readdir(directory, {withFileTypes: true})).filter(entry => entry.isFile() && extname(entry.name).toLowerCase() === '.vpx').map(entry => entry.name).sort();
  const name = candidates.length === 1 ? candidates[0] : candidates.find(name => basename(name, extname(name)).toLowerCase() === basename(directory).toLowerCase());
  if (!name) return null;
  const root = await realpath(tables);
  const path = await realpath(join(directory, name));
  if (!isWithin(root, path)) throw new Error('VPX file is outside the tables folder');
  const hash = createHash('sha256');
  for await (const chunk of createReadStream(path)) hash.update(chunk);
  return hash.digest('hex');
}

export async function removeGame(tables: string, source: string, id: string): Promise<void> {
  if (typeof id !== 'string' || !/^[a-f0-9]{32}$/.test(id)) throw new Error('Refresh history before removing a game');
  const path = await resolveSource(tables, source);
  await withHistoryLock(dirname(path), async () => {
    const text = await readFile(path, 'utf8');
    records(text, source); // Refuse unsupported or malformed histories before any write.
    const document = parseExact(text);
    const index = document.games.findIndex((game: any) => game.id === id);
    if (index < 0) throw new Error('The history changed; refresh and try again');
    document.games.splice(index, 1);
    await atomicWrite(path, stringifyExact(document, true) + '\n');
  });
}
