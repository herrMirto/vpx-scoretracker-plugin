import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, readFile, rm, symlink, utimes, realpath, stat } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { scanScores, removeGame, hashVpx } from '../desktop/scores.ts';
import { parseExact, stringifyExact } from '../desktop/json.ts';
import { withHistoryLock } from '../desktop/storage.ts';
import { loadNvram, decode, loadMap } from '../desktop/nvram.ts';
import { resolveMaps } from '../desktop/host.ts';

export function game(id = 1, scores = [0, 123456]) {
  return {id: id.toString(16).padStart(32, '0'), table: 'AFM.vpx', rom: 'afm_113b', started_at: '2026-11-18T10:00:00Z', ended_at: '2026-11-18T10:01:00Z', duration_ms: 60000,
    source: 'pinmame', scores: scores.map((score, i) => ({player: i + 1, score}))};
}
async function temp(t: {after(fn: () => Promise<void>): void}) {
  const root = await mkdtemp(join(tmpdir(), 'scoretracker-test-'));
  t.after(() => rm(root, {recursive: true, force: true})); return root;
}
test('scan reads the plugin example, keeps player order and zeroes, isolates invalid histories and skips resources', async t => {
  const root = await temp(t);
  for (const name of ['AFM', 'Broken', 'Empty']) await mkdir(join(root, name));
  await writeFile(join(root, 'AFM/scores.json'), JSON.stringify({schema_version: 1, games: [game()]}));
  await writeFile(join(root, 'AFM/AFM.vpx'), 'table');
  await mkdir(join(root, 'AFM/resources'));
  await writeFile(join(root, 'AFM/resources/scores.json'), JSON.stringify({schema_version: 1, games: [game(2)]}));
  await writeFile(join(root, 'Broken/scores.json'), '{');
  await writeFile(join(root, 'Empty/scores.backup.json'), '{}');
  if (process.platform !== 'win32') await symlink(join(root, 'AFM'), join(root, 'Shortcut'));
  const snapshot = await scanScores(root);
  assert.equal(snapshot.games.length, 1); assert.equal(snapshot.sourcesScanned, 2);
  assert.equal(snapshot.vpxFilesFound, 1); assert.equal(snapshot.warnings.length, 1);
  assert.deepEqual(snapshot.games[0].scores, [0, 123456]); assert.equal(snapshot.games[0].gameDuration, 60);
  await writeFile(join(root, 'AFM/scores.json'), await readFile(new URL('../../examples/completed-game.json', import.meta.url)));
  assert.equal((await scanScores(join(root, 'AFM'))).warnings.length, 0);
  assert.equal((await hashVpx(root, 'AFM/scores.json'))?.length, 64);
  const controller = new AbortController();
  await assert.rejects(scanScores(root, async () => { controller.abort(); }, controller.signal), /abort/i);
});
test('deletion uses stable IDs and OS locks, preserves every other integer, and refuses stale IDs or traversal', async t => {
  const root = await temp(t), path = join(root, 'scores.json');
  const text = JSON.stringify({schema_version: 1, games: [game(1), game(2)]}).replace('123456', '9223372036854775807');
  await writeFile(path, text);
  await assert.rejects(removeGame(root, 'scores.json', game(99).id), /history changed/);
  assert.equal(await readFile(path, 'utf8'), text);
  await assert.rejects(removeGame(root, '../settings.json', game().id));
  let deleted = false;
  await withHistoryLock(root, async () => {
    const removal = removeGame(root, 'scores.json', game(2).id).then(() => { deleted = true; });
    // Release via the outer scope; the waiter must not write before unlock.
    t.after(async () => { await removal; });
    await new Promise(resolve => setTimeout(resolve, 75));
    assert.equal(deleted, false); assert.equal(await readFile(path, 'utf8'), text);
  });
  for (let i = 0; i < 100 && !deleted; i++) await new Promise(resolve => setTimeout(resolve, 10));
  assert.ok(deleted);
  const result = await readFile(path, 'utf8');
  assert.ok(result.includes('9223372036854775807'));
  assert.equal(parseExact(result).games.length, 1);
  assert.equal(parseExact(result).games[0].scores[1].score, 9223372036854775807n);
  assert.equal(stringifyExact(parseExact('{"i":9007199254740993,"f":1.25}')), '{"i":9007199254740993,"f":1.25}');
});
test('malformed players, duplicate IDs and future schemas never get rewritten', async t => {
  const root = await temp(t), path = join(root, 'scores.json');
  for (const history of [{schema_version: 2, games: [game()]}, {schema_version: 1, games: [game(), game()]},
    {schema_version: 1, games: [{...game(), scores: [{player: 2, score: 10}]}]}]) {
    const text = JSON.stringify(history); await writeFile(path, text);
    assert.equal((await scanScores(root)).warnings.length, 1);
    await assert.rejects(removeGame(root, 'scores.json', game().id));
    assert.equal(await readFile(path, 'utf8'), text);
  }
});
test('host maps discovery is case-insensitive and NVRAM decoding stays read-only, including grouped ROM fallback', async t => {
  const root = await temp(t), maps = join(root, 'Demo/PinMame/MemMaps');
  await mkdir(join(maps, 'platforms'), {recursive: true});
  await mkdir(join(root, 'Real/pinmame/nvram'), {recursive: true});
  await writeFile(join(root, 'Demo/scores.json'), JSON.stringify({schema_version: 1, games: [game()]}));
  await writeFile(join(maps, 'index.json'), JSON.stringify({demo: 'demo.json'}));
  await writeFile(join(maps, 'platforms/test.json'), JSON.stringify({endian: 'big', memory_layout: [{type: 'nvram', address: '0x100', size: 16}]}));
  await writeFile(join(maps, 'demo.json'), JSON.stringify({_metadata: {platform: 'test'}, high_scores: [{label: 'Grand Champion', initials: {start: '0x100', length: 6, encoding: 'ch', null: 'terminate'}, score: {start: '0x106', length: 3, encoding: 'bcd'}}]}));
  const bytes = Buffer.from([65, 66, 67, 0, 255, 255, 0x12, 0x34, 0x56, 0, 0, 0, 0, 0, 0, 0]);
  const nvram = join(root, 'Real/pinmame/nvram/demo.nv');
  await writeFile(nvram, bytes); await utimes(nvram, 1, 1);
  assert.equal(await realpath(await resolveMaps(root, 'Demo/scores.json', '')), await realpath(maps));
  assert.deepEqual(await loadNvram(root, 'Demo/scores.json', 'demo', ''), {rom: 'demo', highScores: [{label: 'Grand Champion', shortLabel: null, initials: 'ABC', score: 123456}]});
  assert.deepEqual(await readFile(nvram), bytes); assert.equal((await stat(nvram)).mtimeMs, 1000);
  const context = await loadMap(maps, 'demo');
  assert.equal(decode({start: '0x106', length: 3, encoding: 'bcd', endian: 'little'}, context, bytes), 563412);
  assert.equal(decode({start: '0x106', length: 3, encoding: 'bcd', nibble: 'low', scale: 10}, context, bytes), 2460);
  assert.equal(decode({start: '0x200', length: 3}, context, bytes), null);
  await assert.rejects(loadNvram(root, '../outside/scores.json', 'demo', ''));
});
