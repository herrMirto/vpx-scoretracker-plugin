import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, readFile, rm, realpath } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { startServer } from '../desktop/server.ts';
import { request as httpRequest } from 'node:http';

test('desktop API preserves settings, streams histories, guards sessions and deletes only the chosen stable ID', async t => {
  const data = await realpath(await mkdtemp(join(tmpdir(), 'scoretracker-api-')));
  t.after(() => rm(data, {recursive: true, force: true}));
  const tables = join(data, 'Tables'); await mkdir(tables);
  const example = await readFile(new URL('../../examples/completed-game.json', import.meta.url), 'utf8');
  await writeFile(join(tables, 'scores.json'), example);
  await writeFile(join(data, 'settings.json'), JSON.stringify({tablesRoot: tables}));
  let picked: string | null = tables, shown = 0, quit = 0;
  const options = {data, html: '<!doctype html><title>Viewer</title>', version: 'test', hooks: {async choose() {return picked;}, show() {shown++;}, quit() {quit++;}, reveal() {}}};
  let backend = await startServer(options);
  t.after(async () => { await backend.close(); });
  const firstToken = backend.url.split('#token=')[1];
  let token = firstToken;
  const request = (path: string, method = 'GET', body?: unknown, headers: Record<string, string> = {}) => fetch(new URL(path, backend.url), {method, headers: {'X-ScoreTracker-Token': token, ...headers}, body: body === undefined ? undefined : JSON.stringify(body)});
  assert.equal((await fetch(new URL('/api/config', backend.url))).status, 401);
  assert.equal((await request('/api/config', 'GET', undefined, {Origin: 'https://example.org'})).status, 403);
  const hostStatus = await new Promise<number | undefined>((resolve, reject) => {
    const req = httpRequest(new URL('/api/config', backend.url), {headers: {Host: 'evil.invalid', 'X-ScoreTracker-Token': token}}, response => { response.resume(); resolve(response.statusCode); });
    req.on('error', reject); req.end();
  });
  assert.equal(hostStatus, 403);
  assert.equal((await request('/')).status, 200);
  assert.deepEqual(await (await request('/api/config')).json(), {tablesRoot: tables, mapsRoot: '', vpxRoot: '', configurable: true, native: true, readOnly: false});
  const snapshot = await (await request('/api/scores')).json();
  const messages = (await (await request('/api/scores/stream')).text()).trim().split('\n').map(line => JSON.parse(line));
  assert.equal(messages.at(-1).complete, true); assert.deepEqual(messages.at(-1).snapshot.games, snapshot.games);
  assert.equal((await request('/api/show-window', 'POST')).status, 200); assert.equal(shown, 1);
  assert.equal((await request('/api/choose-folder?kind=maps', 'POST')).status, 400);
  assert.equal((await request('/api/choose-folder?kind=vpx', 'POST')).status, 200);
  assert.deepEqual(await (await request('/api/choose-folder', 'POST')).json(), await (await request('/api/config')).json());
  picked = null; assert.equal(await (await request('/api/choose-folder', 'POST')).json(), null);
  assert.equal((await request('/api/config', 'POST', {tablesRoot: tables, vpxRoot: '', mapsRoot: 'old-cache'})).status, 200);
  assert.equal(JSON.parse(await readFile(join(data, 'settings.json'), 'utf8')).mapsRoot, '');
  assert.equal((await request('/api/nvram?rom=afm_113b&scoreSource=../settings.json')).status, 400);
  assert.equal((await request('/api/remove-game', 'POST', {scoreSource: 'scores.json', scoreId: '0'.repeat(32)})).status, 400);
  assert.equal(await readFile(join(tables, 'scores.json'), 'utf8'), example);
  assert.equal((await request('/api/log', 'POST', {event: 'integration-test'})).status, 200);
  assert.match(await readFile(join(data, 'scoretracker-viewer.log'), 'utf8'), /integration-test/);
  assert.equal((await request('/api/remove-game', 'POST', {scoreSource: 'scores.json', scoreId: snapshot.games[0].scoreId})).status, 200);
  assert.equal(JSON.parse(await readFile(join(tables, 'scores.json'), 'utf8')).games.length, snapshot.games.length - 1);
  assert.equal((await request('/api/install', 'POST', {})).status, 404);
  await request('/api/quit', 'POST'); await new Promise(resolve => setTimeout(resolve, 150)); assert.equal(quit, 1);
  await backend.close(); backend = await startServer(options); token = backend.url.split('#token=')[1];
  assert.notEqual(token, firstToken);
  assert.equal((await request('/api/config', 'GET', undefined, {'X-ScoreTracker-Token': firstToken})).status, 401);
  assert.equal((await (await request('/api/config')).json()).tablesRoot, tables);
});

test('service mode is read-only for every mutation endpoint', async t => {
  const data = await mkdtemp(join(tmpdir(), 'scoretracker-service-'));
  t.after(() => rm(data, {recursive: true, force: true}));
  const backend = await startServer({data, html: '<title>Viewer</title>', version: 'test', service: {tablesRoot: data, mapsRoot: '', vpxRoot: ''}});
  t.after(() => backend.close());
  const response = await fetch(new URL('/api/config', backend.url)); assert.equal((await response.json()).readOnly, true);
  for (const endpoint of ['remove-game', 'config', 'quit', 'choose-folder', 'show-window', 'log', 'reveal-log']) {
    assert.equal((await fetch(new URL(`/api/${endpoint}`, backend.url), {method: 'POST', body: '{}'})).status, 403);
  }
});
