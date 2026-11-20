import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, readFile, rm } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { _electron as electron } from 'playwright-core';
import electronPath from 'electron';
import { existsSync } from 'node:fs';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
const root = fileURLToPath(new URL('../', import.meta.url));
const data = await mkdtemp(join(tmpdir(), 'scoretracker-desktop-'));
const tables = join(data, 'Tables');
await mkdir(tables);
const fixture = JSON.parse(await readFile(new URL('../../examples/completed-game.json', import.meta.url), 'utf8'));
const originalId = fixture.games[0].id;
fixture.games.push(...[0, 200, 400, 100, 600, 500].map((score, index) => ({
  id: (index + 1).toString(16).padStart(32, '0'), table: 'Attack from Mars.vpx', rom: 'afm_113b',
  started_at: '2026-11-18T10:00:00.000Z', ended_at: `2026-11-18T10:0${index + 1}:00.000Z`, duration_ms: 60000,
  source: 'pinmame', scores: [{player: 1, score}],
})));
const fixtureText = JSON.stringify(fixture);
await writeFile(join(tables, 'scores.json'), fixtureText);
await writeFile(join(data, 'settings.json'), JSON.stringify({tablesRoot: tables, vpxRoot: '', mapsRoot: ''}));
const packaged = resolve(root, '../build/viewer', process.platform === 'darwin'
  ? `${process.arch === 'arm64' ? 'mac-arm64' : 'mac'}/ScoreTracker.app/Contents/MacOS/ScoreTracker`
  : process.platform === 'win32' ? 'win-unpacked/ScoreTracker.exe'
    : `${process.arch === 'arm64' ? 'linux-arm64-unpacked' : 'linux-unpacked'}/scoretracker-viewer`);
const executable = process.env.SCORETRACKER_APP ?? (existsSync(packaged) ? packaged : undefined);
const options = executable ? {executablePath: executable, args: ['--data-dir', data]} : {executablePath: electronPath, args: [root, '--data-dir', data]};
let application;
try {
  application = await electron.launch({...options, timeout: 30000});
  const page = await application.firstWindow();
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  await page.route('https://api.vpinplay.com:8888/**', route => route.fulfill({status: 200, contentType: 'application/json', body: JSON.stringify({items: route.request().url().includes('/by-rom/afm_113b') ? [{vpsId: 'fixture-afm', vpsdb: {name: 'Attack from Mars', manufacturer: 'Bally', year: 1995}}] : []})}));
  await page.route('https://raw.githubusercontent.com/superhac/vpinmediadb/**', route => route.fulfill({status: 200, contentType: 'image/png', body: Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=', 'base64')}));
  await page.route('**/api/nvram?**', route => route.fulfill({status: 200, contentType: 'application/json', body: JSON.stringify({rom:'afm_113b', highScores:[{label:'Grand Champion',shortLabel:'GC',initials:'AAA',score:2000},{label:'First Place',shortLabel:'1st',initials:'BBB',score:1000}]})}));
  await page.reload();
  await page.waitForFunction(() => document.body.textContent.includes('Original Table'), undefined, {timeout: 15000});
  assert.equal(await page.evaluate(() => typeof window.require), 'undefined');
  const preferences = await application.evaluate(({BrowserWindow}) => {
    const p = BrowserWindow.getAllWindows()[0].webContents.getLastWebPreferences();
    return {sandbox: p.sandbox, contextIsolation: p.contextIsolation, nodeIntegration: p.nodeIntegration};
  });
  assert.deepEqual(preferences, {sandbox: true, contextIsolation: true, nodeIntegration: false});
  const base = (await readFile(join(data, 'session.url'), 'utf8')).split('#')[0];
  const external = await page.evaluate(async () => {
    const response = await fetch('/api/config', {headers: {'X-ScoreTracker-Token': sessionStorage.getItem('scoretracker.session')}});
    return response.json();
  });
  assert.equal(external.native, true); assert.equal(external.readOnly, false);
  assert.ok(base.startsWith('http://127.0.0.1:'));
  await application.evaluate(({dialog}, path) => {
    dialog.showOpenDialog = async () => ({canceled: false, filePaths: [path]});
  }, tables);
  const picked = await page.evaluate(async () => {
    const response = await fetch('/api/choose-folder', {method: 'POST', headers: {'X-ScoreTracker-Token': sessionStorage.getItem('scoretracker.session')}});
    return response.json();
  });
  assert.equal(picked.tablesRoot, tables);
  await promisify(execFile)(options.executablePath, [...(executable ? [] : [root]), '--data-dir', data], {timeout: 10000});
  assert.equal(await application.evaluate(({BrowserWindow}) => BrowserWindow.getAllWindows().length), 1);
  await mkdir(resolve(root, '../build/viewer'), {recursive: true});
  await page.screenshot({path: resolve(root, '../build/viewer/desktop-smoke.png')});
  assert.deepEqual(errors, []);
  const original = await readFile(join(tables, 'scores.json'), 'utf8');
  assert.equal(original, fixtureText);
  assert.equal(await page.locator('.brand strong').textContent(), 'VPX ScoreTracker Viewer');
  assert.equal(await page.locator('footer').innerText(), 'Scores stored locally');
  assert.equal(await page.locator('.table-tile').count(), 2);
  assert.doesNotMatch(await page.locator('body').innerText(), /LAYOUT PREVIEW|LOCAL HISTORY SNAPSHOT|SAMPLE DATA/);
  await page.locator('#search').fill('missing-table'); assert.equal(await page.locator('.table-tile').count(), 0);
  await page.locator('#search').fill('afm_113b'); assert.equal(await page.locator('.table-tile').count(), 1);
  await page.locator('#search').fill('');
  await page.locator('#sort').selectOption('games');
  assert.match(await page.locator('.table-tile').first().innerText(), /Attack from Mars/);
  await page.screenshot({path: resolve(root, '../build/viewer/desktop-home.png'), fullPage: true});
  await page.locator('.table-tile').first().click();
  await page.waitForSelector('.chart-point');
  await page.waitForFunction(() => document.querySelector('.detail-kpis')?.textContent.includes('40%'));
  assert.equal(await page.locator('.chart-point').count(), 6);
  assert.match(await page.locator('.detail-kpis').innerText(), /Percentage remaining to first place/);
  assert.match(await page.locator('.leaderboard').innerText(), /Table high scores/);
  assert.equal(await page.locator('.wheel-art.identity-art img').count(), 1);
  await page.waitForFunction(() => document.querySelector('.identity-art')?.classList.contains('wheel-loaded'));
  await page.locator('[data-range="5"]').click(); assert.equal(await page.locator('.chart-point').count(), 5);
  await page.locator('[data-game="0"]').click(); assert.equal(await page.locator('.chart-point').count(), 6);
  assert.match(await page.locator('.chart-selection').innerText(), /0 points/);
  await page.locator('[data-tab="history"]').click(); assert.equal(await page.locator('.chart-block').isVisible(), false);
  await page.locator('[data-tab="overview"]').click();
  await page.screenshot({path: resolve(root, '../build/viewer/desktop-detail.png'), fullPage: true});
  await page.setViewportSize({width: 420, height: 900});
  assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
  await page.setViewportSize({width: 1200, height: 800});
  await page.locator('#show-folders').click(); await page.waitForSelector('.folders-modal');
  await page.keyboard.press('Escape'); assert.equal(await page.locator('.folders-modal').count(), 0);
  await page.locator('#back').click();
  await page.locator('#search').fill('Original Table'); await page.locator('.table-tile').click();
  await page.waitForSelector('.history-table tbody tr');
  assert.equal(await page.locator('.history-table tbody tr').count(), 2);
  assert.match(await page.locator('.history-table').innerText(), /P2/);
  assert.equal(await page.locator('.history-table tbody tr').first().locator('.score').innerText(), '0');
  const removed = await page.evaluate(async id => {
    const response = await fetch('/api/remove-game', {method: 'POST', headers: {'Content-Type': 'application/json', 'X-ScoreTracker-Token': sessionStorage.getItem('scoretracker.session')},
      body: JSON.stringify({scoreSource: 'scores.json', scoreId: id})});
    return response.status;
  }, originalId);
  assert.equal(removed, 200);
  assert.equal(JSON.parse(await readFile(join(tables, 'scores.json'), 'utf8')).games.length, 6);
  assert.deepEqual(errors, []);
  await application.close(); application = undefined;
  assert.equal(await readFile(join(data, 'session.url'), 'utf8').catch(() => null), null);
  console.log('PASS: desktop layout, search/sort, live wheels and NVRAM, chart selection/range, zero and multiplayer scores, folders, API, single instance, isolated renderer and clean shutdown');
} finally {
  await application?.close();
  await rm(data, {recursive: true, force: true});
}
