// Node-only entry point for API integration tests and a read-only service.
import { readFile, writeFile, rm } from 'node:fs/promises';
import { join } from 'node:path';
import { startServer } from './server.ts';
import { argument, dataDirectory } from './paths.ts';
const service = process.argv.includes('--service');
if (!service && !process.argv.includes('--no-open')) throw new Error('Use npm start for the desktop application');
const data = dataDirectory();
const session = join(data, 'session.url');
const packageInfo = JSON.parse(await readFile(new URL('../../package.json', import.meta.url), 'utf8'));
const html = await readFile(new URL('../../dist/index.html', import.meta.url), 'utf8');
const [host, port] = (argument('--listen') ?? '127.0.0.1:8080').split(':');
const backend = await startServer({data, html, version: packageInfo.version,
  ...(service ? {host, port: Number(port), service: {tablesRoot: argument('--tables-root') ?? '/userdata/roms/vpinball', mapsRoot: argument('--maps-root') ?? '', vpxRoot: argument('--vpx-root') ?? '/usr/bin/vpinball'}} : {}),
  hooks: {async choose() { return null; }, show() {}, reveal() {}, quit() { void stop(); }}});
if (!service) await writeFile(session, backend.url, {mode: 0o600});
console.log(`ScoreTracker: ${backend.url.split('#')[0]}`);
let stopping = false;
async function stop() {
  if (stopping) return; stopping = true;
  await backend.close();
  if (!service) await rm(session, {force: true});
}
process.on('SIGINT', () => { void stop(); });
process.on('SIGTERM', () => { void stop(); });
