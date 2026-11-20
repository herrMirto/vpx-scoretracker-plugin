import { createServer, type ServerResponse, type IncomingMessage } from 'node:http';
import { randomBytes, createHash } from 'node:crypto';
import { mkdir, readFile, appendFile, stat, rename } from 'node:fs/promises';
import { join } from 'node:path';
import { once } from 'node:events';
import { scanScores, hashVpx, removeGame } from './scores.ts';
import { loadNvram } from './nvram.ts';
import { atomicWrite } from './storage.ts';
import { detectVpx } from './host.ts';
import { stringifyExact } from './json.ts';

export interface Settings { tablesRoot: string; mapsRoot: string; vpxRoot: string }
export interface DesktopHooks {
  choose(kind: string, start: string): Promise<string | null>;
  show(): void;
  quit(): void;
  reveal(path: string): void;
}
interface Options {
  data: string;
  html: string;
  version: string;
  service?: Settings;
  host?: string;
  port?: number;
  hooks?: DesktopHooks;
}
export async function startServer(options: Options) {
  const desktop = !options.service;
  const token = desktop ? randomBytes(32).toString('hex') : '';
  const configPath = join(options.data, 'settings.json');
  const logPath = join(options.data, 'scoretracker-viewer.log');
  let settings: Settings = options.service ?? {tablesRoot: '', mapsRoot: '', vpxRoot: ''};
  if (desktop) {
    await mkdir(options.data, {recursive: true, mode: 0o700});
    try {
      const saved = JSON.parse(await readFile(configPath, 'utf8'));
      settings = {tablesRoot: typeof saved.tablesRoot === 'string' ? saved.tablesRoot : '', vpxRoot: typeof saved.vpxRoot === 'string' ? saved.vpxRoot : '', mapsRoot: ''};
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code !== 'ENOENT') throw error;
      settings.vpxRoot = await detectVpx();
    }
  }
  let logQueue = Promise.resolve();
  function log(value: unknown): Promise<void> {
    const write = logQueue.then(async () => {
      if (((await stat(logPath).catch(() => null))?.size ?? 0) > 2 * 1024 * 1024) await rename(logPath, join(options.data, 'scoretracker-viewer.previous.log'));
      await appendFile(logPath, JSON.stringify(value) + '\n', {mode: 0o600});
    });
    logQueue = write.catch(() => {}); return write;
  }
  let saveQueue = Promise.resolve();
  function save(value: Settings): Promise<void> {
    const write = saveQueue.then(async () => { await atomicWrite(configPath, JSON.stringify(value, null, 2)); settings = value; });
    saveQueue = write.catch(() => {}); return write;
  }
  const config = () => ({...settings, configurable: desktop, native: desktop, readOnly: !desktop});
  let selecting = false;
  let address = '';
  function send(response: ServerResponse, value: unknown, status = 200) {
    response.writeHead(status, {'Content-Type': 'application/json'}); response.end(stringifyExact(value));
  }
  async function body(request: IncomingMessage): Promise<any> {
    let size = 0; const chunks: Buffer[] = [];
    for await (const chunk of request) { size += chunk.length; if (size > 65536) throw new Error('Request too large'); chunks.push(chunk); }
    return JSON.parse(Buffer.concat(chunks).toString() || '{}');
  }
  const scriptHash = createHash('sha256').update(options.html.match(/<script type="module">([\s\S]*?)<\/script>/)?.[1] ?? '').digest('base64');
  const server = createServer((request, response) => {
    response.setHeader('Cache-Control', 'no-store');
    response.setHeader('X-Frame-Options', 'DENY');
    response.setHeader('X-Content-Type-Options', 'nosniff');
    response.setHeader('Content-Security-Policy', `default-src 'self'; script-src 'self' 'sha256-${scriptHash}'; style-src 'self' 'unsafe-inline'; img-src 'self' https: data:; connect-src 'self' https://api.vpinplay.com:8888; frame-src 'none'; object-src 'none'; base-uri 'none'`);
    const run = async () => {
      const url = new URL(request.url ?? '/', `http://${address}`);
      const path = url.pathname, method = request.method ?? 'GET';
      if (desktop && (request.headers.host !== address || (request.headers.origin !== undefined && request.headers.origin !== `http://${address}`))) return send(response, {error: 'Use the ScoreTracker address'}, 403);
      if (desktop && path.startsWith('/api/') && request.headers['x-scoretracker-token'] !== token) return send(response, {error: 'Session expired. Reopen ScoreTracker.'}, 401);
      if (!desktop && method !== 'GET' && method !== 'HEAD') return send(response, {error: 'This action is only available in the desktop Viewer'}, 403);
      if ((method === 'GET' || method === 'HEAD') && path === '/') {
        response.writeHead(200, {'Content-Type': 'text/html; charset=utf-8'}); return response.end(method === 'HEAD' ? undefined : options.html);
      }
      const get = method === 'GET';
      if (get && path === '/api/health') return send(response, {status: 'ok', version: options.version});
      if (get && path === '/api/config') return send(response, config());
      if (get && path === '/api/scores') return send(response, await scanScores(settings.tablesRoot));
      if (get && path === '/api/scores/stream') {
        response.writeHead(200, {'Content-Type': 'application/x-ndjson'});
        const controller = new AbortController();
        response.on('close', () => controller.abort());
        const write = async (value: unknown) => {
          controller.signal.throwIfAborted();
          if (!response.write(stringifyExact(value) + '\n')) await once(response, 'drain', {signal: controller.signal});
        };
        const started = Date.now();
        try {
          const snapshot = await scanScores(settings.tablesRoot, value => write({snapshot: value, complete: false}), controller.signal);
          await write({snapshot, complete: true});
        } catch (error) { if (!controller.signal.aborted) await write({error: String(error)}); }
        finally {
          response.end();
          if (desktop) await log({event: 'scores.filesystem_scan', elapsedMs: Date.now() - started, cancelled: controller.signal.aborted});
        }
        return;
      }
      const source = url.searchParams.get('scoreSource') ?? '';
      if (get && path === '/api/vpx-hash') return send(response, await hashVpx(settings.tablesRoot, source));
      if (get && path === '/api/nvram') return send(response, await loadNvram(settings.tablesRoot, source, url.searchParams.get('rom') ?? '', settings.vpxRoot, settings.mapsRoot));
      if (method === 'POST') {
        if (path === '/api/config') {
          const value = await body(request);
          if (typeof value.tablesRoot !== 'string' || typeof value.vpxRoot !== 'string') throw new Error('Invalid folder settings');
          const {realpath, readdir} = await import('node:fs/promises');
          const root = await realpath(value.tablesRoot); await readdir(root);
          await save({tablesRoot: root, mapsRoot: '', vpxRoot: value.vpxRoot}); return send(response, config());
        }
        if (path === '/api/choose-folder') {
          const kind = url.searchParams.get('kind') ?? 'tables';
          if (!['tables', 'vpx'].includes(kind)) throw new Error('Unknown folder kind');
          if (selecting) return send(response, {error: 'Folder selection is already open'}, 409);
          selecting = true;
          try {
            const path = await options.hooks?.choose(kind, kind === 'tables' ? settings.tablesRoot : settings.vpxRoot);
            if (!path) return send(response, null);
            if (kind === 'vpx') return send(response, {path});
            await save({...settings, tablesRoot: path}); return send(response, config());
          } finally { selecting = false; }
        }
        if (path === '/api/remove-game') {
          const value = await body(request);
          if (typeof value.scoreSource !== 'string') throw new Error('Invalid history source');
          await removeGame(settings.tablesRoot, value.scoreSource, value.scoreId); return send(response, {removed: true});
        }
        if (path === '/api/show-window') { options.hooks?.show(); return send(response, {shown: true}); }
        if (path === '/api/log') { await log(await body(request)); return send(response, null); }
        if (path === '/api/reveal-log') { await appendFile(logPath, ''); options.hooks?.reveal(logPath); return send(response, null); }
        if (path === '/api/quit') { send(response, {stopped: true}); setTimeout(() => { options.hooks?.quit(); }, 100); return; }
      }
      send(response, {error: 'Not found'}, 404);
    };
    void run().catch(error => {
      if (response.headersSent) { response.destroy(); return; }
      send(response, {error: String(error)}, 400);
    });
  });
  server.listen(options.port ?? 0, desktop ? '127.0.0.1' : options.host ?? '127.0.0.1');
  await once(server, 'listening');
  const bound = server.address();
  if (!bound || typeof bound === 'string') throw new Error('Cannot bind Viewer API');
  address = `127.0.0.1:${bound.port}`;
  const url = `http://${address}/${desktop ? `#token=${token}` : ''}`;
  return {url, server, async close() {
    server.closeAllConnections();
    await new Promise<void>((resolve, reject) => server.close(error => error ? reject(error) : resolve()));
    await Promise.all([logQueue, saveQueue]);
  }};
}
