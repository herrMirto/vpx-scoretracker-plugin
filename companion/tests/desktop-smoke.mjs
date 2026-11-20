import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, readFile, rm } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { _electron as electron } from 'playwright-core';
import { existsSync, writeFileSync } from 'node:fs';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { bounded, closeDesktop } from './desktop-support.mjs';
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
// Importing the npm electron module can synchronously download another runtime.
// Exercise the app produced by packaging instead, without a development fallback.
const executable = process.env.SCORETRACKER_APP ?? packaged;
const options = {executablePath: executable, args: ['--data-dir', data]};
const diagnostics = resolve(root, '../build/desktop-smoke');
await mkdir(diagnostics, {recursive: true});
let application;
let page;
const messages = [];
function record(message) { messages.push(String(message)); if (messages.length > 200) messages.shift(); }
async function closeApplication() {
  if (!application) return;
  const current = application;
  try { await closeDesktop(current); }
  finally { application = undefined; }
}
let stage = 'launch packaged app';
const deadline = setTimeout(() => {
  console.error(`Desktop smoke test exceeded 120 seconds during: ${stage}`);
  writeFileSync(join(diagnostics, 'diagnostics.log'), [...messages, `Global timeout during: ${stage}`].join('\n'));
  application?.process().kill('SIGKILL');
  process.exit(1);
}, 120_000);
function checkpoint(value) { stage = value; const message = `Desktop smoke: ${stage}`; record(message); console.log(message); }
try {
  assert.ok(existsSync(executable), `Packaged app missing: ${executable}. Run npm run build and npm run package first.`);
  checkpoint('launch packaged app');
  application = await electron.launch({...options, timeout: 30000});
  application.context().setDefaultTimeout(15000);
  application.context().setDefaultNavigationTimeout(30000);
  application.on('console', message => record(`main: ${message.text()}`));
  application.process().stderr?.on('data', chunk => record(`stderr: ${chunk}`));
  const context = application.context();
  await context.tracing.start({screenshots: true, snapshots: true});
  // Any request made before routing was attached must not seed online metadata
  // into the fixture reload. Keep sessionStorage, which holds the API token.
  await context.addInitScript(() => { try { localStorage.clear(); } catch { /* about:blank */ } });
  checkpoint('install fixture routes');
  // Catch requests from every window, including the initial page, before reload.
  await context.route('**/*', route => {
    const url = new URL(route.request().url());
    if (url.hostname === 'api.vpinplay.com') return route.fulfill({status: 200, contentType: 'application/json', body: JSON.stringify({items: url.pathname.includes('/by-rom/afm_113b') ? [{vpsId: 'fixture-afm', vpsdb: {name: 'Attack from Mars', manufacturer: 'Bally', year: 1995}}] : []})});
    if (url.hostname === 'raw.githubusercontent.com') return route.fulfill({status: 200, contentType: 'image/png', body: Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=', 'base64')});
    if (url.hostname === '127.0.0.1' && url.pathname === '/api/nvram') return route.fulfill({status: 200, contentType: 'application/json', body: JSON.stringify({rom:'afm_113b', highScores:[{label:'Grand Champion',shortLabel:'GC',initials:'AAA',score:2000},{label:'First Place',shortLabel:'1st',initials:'BBB',score:1000}]})});
    if (url.hostname === '127.0.0.1') return route.continue();
    // The smoke test is offline; unexpected external services must not stall it.
    return route.abort('blockedbyclient');
  });
  checkpoint('wait for initial app navigation');
  page = await application.firstWindow({timeout: 15000});
  const errors = [];
  page.on('pageerror', error => { errors.push(String(error)); record(`pageerror: ${error}`); });
  page.on('console', message => record(`renderer: ${message.text()}`));
  page.on('crash', () => record('renderer crashed'));
  page.on('requestfailed', request => record(`request failed: ${request.url().split('#')[0]} ${request.failure()?.errorText}`));
  const session = await readFile(join(data, 'session.url'), 'utf8');
  const base = session.split('#')[0];
  const origin = new URL(base).origin;
  // firstWindow means BrowserWindow was created, not that loadURL finished.
  // Reloading before did-finish-load aborts the startup loadURL promise.
  await page.waitForURL(url => url.origin === origin, {waitUntil: 'load', timeout: 15000});
  await bounded(() => application.evaluate(async ({BrowserWindow}) => {
    const contents = BrowserWindow.getAllWindows()[0].webContents;
    if (contents.isLoadingMainFrame()) await new Promise(resolve => contents.once('did-finish-load', resolve));
  }), 15000, 'Initial Electron navigation');
  checkpoint('reload with fixture routes');
  await page.reload({waitUntil: 'domcontentloaded', timeout: 15000});
  checkpoint('wait for fixture collection');
  await page.waitForFunction(() => document.body.textContent.includes('Original Table'), undefined, {timeout: 15000});
  checkpoint('check renderer sandbox');
  assert.equal(await page.evaluate(() => typeof window.require), 'undefined');
  const preferences = await bounded(() => application.evaluate(({BrowserWindow}) => {
    const p = BrowserWindow.getAllWindows()[0].webContents.getLastWebPreferences();
    return {sandbox: p.sandbox, contextIsolation: p.contextIsolation, nodeIntegration: p.nodeIntegration};
  }), 15000, 'Renderer preferences');
  assert.deepEqual(preferences, {sandbox: true, contextIsolation: true, nodeIntegration: false});
  checkpoint('check local API configuration');
  const external = await bounded(() => page.evaluate(async () => {
    const response = await fetch('/api/config', {headers: {'X-ScoreTracker-Token': sessionStorage.getItem('scoretracker.session')}});
    return response.json();
  }), 15000, 'Configuration API');
  assert.equal(external.native, true); assert.equal(external.readOnly, false);
  assert.ok(base.startsWith('http://127.0.0.1:'));
  checkpoint('choose fixture folder');
  await bounded(() => application.evaluate(({dialog}, path) => {
    dialog.showOpenDialog = async () => ({canceled: false, filePaths: [path]});
  }, tables), 15000, 'Folder dialog mock');
  const picked = await bounded(() => page.evaluate(async () => {
    const response = await fetch('/api/choose-folder', {method: 'POST', headers: {'X-ScoreTracker-Token': sessionStorage.getItem('scoretracker.session')}});
    return response.json();
  }), 15000, 'Folder selection API');
  assert.equal(picked.tablesRoot, tables);
  checkpoint('check single instance');
  await promisify(execFile)(options.executablePath, ['--data-dir', data], {timeout: 10000});
  assert.equal(await application.evaluate(({BrowserWindow}) => BrowserWindow.getAllWindows().length), 1);
  await page.screenshot({path: join(diagnostics, 'desktop-smoke.png')});
  assert.deepEqual(errors, []);
  const original = await readFile(join(tables, 'scores.json'), 'utf8');
  assert.equal(original, fixtureText);
  checkpoint('check collection');
  assert.equal(await page.locator('.brand strong').textContent(), 'VPX ScoreTracker Viewer');
  assert.equal(await page.locator('footer').innerText(), 'Scores stored locally');
  assert.equal(await page.locator('.table-tile').count(), 2);
  assert.doesNotMatch(await page.locator('body').innerText(), /LAYOUT PREVIEW|LOCAL HISTORY SNAPSHOT|SAMPLE DATA/);
  await page.locator('#search').fill('missing-table'); assert.equal(await page.locator('.table-tile').count(), 0);
  await page.locator('#search').fill('afm_113b'); assert.equal(await page.locator('.table-tile').count(), 1);
  await page.locator('#search').fill('');
  await page.locator('#sort').selectOption('games');
  assert.match(await page.locator('.table-tile').first().innerText(), /Attack from Mars/);
  await page.screenshot({path: join(diagnostics, 'desktop-home.png'), fullPage: true});
  await page.locator('.table-tile').first().click();
  checkpoint('check table details');
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
  await page.screenshot({path: join(diagnostics, 'desktop-detail.png'), fullPage: true});
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
  checkpoint('close packaged app');
  await closeApplication();
  assert.equal(await readFile(join(data, 'session.url'), 'utf8').catch(() => null), null);
  console.log('PASS: desktop layout, search/sort, live wheels and NVRAM, chart selection/range, zero and multiplayer scores, folders, API, single instance, isolated renderer and clean shutdown');
} catch (error) {
  // Report the actual failing operation before graceful Electron cleanup can stall.
  console.error(`Desktop smoke failed during: ${stage}`, error);
  record(String(error?.stack ?? error));
  if (page) {
    await page.screenshot({path: join(diagnostics, 'failure.png'), timeout: 3000}).catch(() => {});
    await bounded(() => page.content().then(html => writeFile(join(diagnostics, 'failure.html'), html)), 3000, 'Failure HTML').catch(() => {});
  }
  if (application) await bounded(() => application.context().tracing.stop({path: join(diagnostics, 'trace.zip')}), 5000, 'Failure trace').catch(() => {});
  throw error;
} finally {
  try {
    await writeFile(join(diagnostics, 'diagnostics.log'), messages.join('\n'));
    await closeApplication().catch(error => console.error('Desktop smoke cleanup:', error));
    await rm(data, {recursive: true, force: true});
  } finally { clearTimeout(deadline); }
}
