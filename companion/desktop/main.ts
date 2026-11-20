import { app, BrowserWindow, dialog, shell, Menu } from 'electron';
import { readFile, writeFile, rm } from 'node:fs/promises';
import { join } from 'node:path';
import { mkdirSync } from 'node:fs';
import { startServer } from './server.ts';
import { argument, dataDirectory } from './paths.ts';

const data = dataDirectory();
mkdirSync(data, {recursive: true, mode: 0o700});
app.setPath('userData', data);
app.setName('ScoreTracker');
if (!app.requestSingleInstanceLock()) app.quit();
else {
  let window: BrowserWindow | null = null;
  let backend: Awaited<ReturnType<typeof startServer>> | undefined;
  let quitting = false;
  const show = () => { if (window?.isMinimized()) window.restore(); window?.show(); window?.focus(); };
  app.on('second-instance', show);
  app.on('activate', show);
  app.on('window-all-closed', () => app.quit());
  app.on('before-quit', event => {
    if (quitting || !backend) return;
    event.preventDefault(); quitting = true;
    void backend.close().then(() => rm(join(data, 'session.url'), {force: true})).finally(() => app.quit());
  });
  process.on('SIGINT', () => app.quit());
  process.on('SIGTERM', () => app.quit());
  void app.whenReady().then(async () => {
    Menu.setApplicationMenu(Menu.buildFromTemplate([
      ...(process.platform === 'darwin' ? [{role: 'appMenu' as const}] : []),
      {role: 'editMenu'}, {role: 'viewMenu'}, {role: 'windowMenu'},
    ]));
    backend = await startServer({data, html: await readFile(join(app.getAppPath(), 'dist/index.html'), 'utf8'), version: app.getVersion(), hooks: {
      show, quit() { app.quit(); }, reveal(path) { shell.showItemInFolder(path); },
      async choose(kind, start) {
        const options = {title: kind === 'vpx' ? 'Choose your Visual Pinball installation' : 'Choose your VPX tables folder', defaultPath: start || undefined, properties: ['openDirectory' as const]};
        const result = window ? await dialog.showOpenDialog(window, options) : await dialog.showOpenDialog(options);
        return result.canceled ? null : result.filePaths[0] ?? null;
      },
    }});
    await writeFile(join(data, 'session.url'), backend.url, {mode: 0o600});
    if (process.argv.includes('--no-open')) return;
    const dev = !app.isPackaged ? argument('--dev-url') : undefined;
    if (dev && new URL(dev).origin !== 'http://127.0.0.1:1420') throw new Error('Invalid development URL');
    const target = dev ? `${dev}/#token=${backend.url.split('#token=')[1]}` : backend.url;
    if (dev) {
      let ready = false;
      for (let attempt = 0; attempt < 200; attempt++) {
        try { if ((await fetch(dev)).ok) { ready = true; break; } } catch { /* Vite is starting. */ }
        await new Promise(resolve => setTimeout(resolve, 100));
      }
      if (!ready) throw new Error('The development server did not start');
    }
    window = new BrowserWindow({title: 'VPX ScoreTracker Viewer', width: 1200, height: 800, minWidth: 640, minHeight: 480,
      webPreferences: {nodeIntegration: false, contextIsolation: true, sandbox: true, webviewTag: false}});
    const origin = new URL(target).origin;
    window.webContents.on('will-navigate', (event, url) => { if (new URL(url).origin !== origin) event.preventDefault(); });
    window.webContents.setWindowOpenHandler(() => ({action: 'deny'}));
    window.webContents.session.setPermissionRequestHandler((_, __, callback) => callback(false));
    await window.loadURL(target);
  }).catch(error => {
    console.error(error);
    dialog.showErrorBox('ScoreTracker', String(error));
    app.quit();
  });
}
