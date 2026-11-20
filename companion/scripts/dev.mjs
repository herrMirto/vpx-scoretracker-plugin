// Run the real desktop backend and Vite together, with a loopback-only API proxy.
import { spawn } from 'node:child_process';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createServer } from 'vite';
import electron from 'electron';
const root = fileURLToPath(new URL('../', import.meta.url));
const data = await mkdtemp(join(tmpdir(), 'scoretracker-dev-'));
const build = spawn(process.platform === 'win32' ? 'npm.cmd' : 'npm', ['run', 'build'], {cwd:root, stdio:'inherit', shell:process.platform === 'win32'});
if (await new Promise(resolve => build.once('exit', resolve)) !== 0) { await rm(data, {recursive:true, force:true}); process.exit(1); }
const backend = spawn(electron, [root, '--dev-url', 'http://127.0.0.1:1420', '--data-dir', data], {cwd:root, stdio:'inherit'});
let vite;
let exiting = false;
async function cleanup() {
  if (exiting) return; exiting = true;
  await vite?.close();
  if (backend.exitCode === null) { backend.kill('SIGTERM'); await new Promise(resolve => backend.once('exit', resolve)); }
  await rm(data, {recursive:true, force:true});
}
process.on('SIGINT', () => { void cleanup(); });
process.on('SIGTERM', () => { void cleanup(); });
try {
  let session;
  for (let attempt = 0; attempt < 1200; attempt++) {
    try { session = await readFile(join(data, 'session.url'), 'utf8'); break; } catch { /* Electron is starting */ }
    if (backend.exitCode !== null) throw new Error('The Viewer backend exited');
    await new Promise(resolve => setTimeout(resolve, 100));
  }
  if (!session) throw new Error('The Viewer backend did not start');
  const [target, token] = session.split('/#token=');
  vite = await createServer({root:fileURLToPath(new URL('../', import.meta.url)), server:{host:'127.0.0.1', port:1420, strictPort:true,
    proxy:{'/api': {target, changeOrigin:true, headers:{'X-ScoreTracker-Token':token}, configure(proxy) {
      proxy.on('proxyReq', req => { req.setHeader('Origin', target); });
    }}}}});
  await vite.listen(); vite.printUrls();
  backend.once('exit', () => { void cleanup(); });
} catch(error) { console.error(error); await cleanup(); process.exitCode = 1; }
