import { readdir, readFile, stat } from 'node:fs/promises';
import { basename, dirname, join } from 'node:path';
import { homedir } from 'node:os';

async function exists(path: string, directory = true): Promise<boolean> {
  try { const info = await stat(path); return directory ? info.isDirectory() : info.isFile(); } catch { return false; }
}
async function entries(path: string): Promise<string[]> {
  try { return (await readdir(path)).sort().map(name => join(path, name)); } catch { return []; }
}
export async function directory(path: string): Promise<string | null> {
  if (await exists(path)) return path;
  const parent = dirname(path);
  if (parent === path) return null;
  const resolved = await directory(parent);
  if (!resolved) return null;
  for (const child of await entries(resolved)) if (basename(child).toLowerCase() === basename(path).toLowerCase() && await exists(child)) return child;
  return null;
}
export async function resolveVpx(path: string): Promise<string | null> {
  if (process.platform === 'darwin') {
    const isApp = async (p: string) => /^vpinballx.*\.app$/i.test(basename(p)) && await exists(join(p, 'Contents/Resources'));
    if (await isApp(path)) return path;
    for (const app of [...await entries(path), ...await entries(join(path, 'build'))].sort()) if (await isApp(app)) return app;
  } else {
    const candidates = [path, ...['bin', 'build', 'build/bin', 'build/Release', 'build/Release/bin'].map(p => join(path, p)), ...await entries(path)];
    for (const candidate of candidates.sort()) {
      for (const file of await entries(candidate)) {
        if (!/^vpinballx/i.test(basename(file)) || !(await exists(file, false))) continue;
        if (process.platform === 'win32' ? /\.exe$/i.test(file) : (await stat(file)).mode & 0o111) return candidate;
      }
    }
  }
  return null;
}
export async function detectVpx(): Promise<string> {
  const home = homedir();
  const roots = process.platform === 'darwin' ? ['/Applications', join(home, 'Applications')]
    : process.platform === 'win32' ? [process.env.ProgramFiles, process.env['ProgramFiles(x86)']].filter(Boolean).flatMap(p => [join(p!, 'VPinball'), join(p!, 'Visual Pinball')]).concat(['C:\\Visual Pinball', join(home, 'Visual Pinball'), join(home, 'Documents/Visual Pinball')])
    : ['/opt/vpinball', '/usr/local/share/vpinball', join(home, '.local/share/VPinballX')];
  for (const path of [...roots, join(home, 'vpinball'), join(home, 'VPinballX')]) {
    const found = await resolveVpx(path); if (found) return found;
  }
  return '';
}
async function findIni(vpx: string): Promise<string | null> {
  const home = homedir();
  const base = process.platform === 'darwin' ? join(home, 'Library/Application Support')
    : process.platform === 'win32' ? process.env.APPDATA ?? home : process.env.XDG_DATA_HOME ?? join(home, '.local/share');
  const versions = (await entries(join(base, 'VPinballX'))).filter(path => /^\d+\.\d+$/.test(basename(path)))
    .sort((a, b) => { const x = basename(a).split('.').map(Number), y = basename(b).split('.').map(Number); return y[0] - x[0] || y[1] - x[1]; });
  for (const version of versions) if (await exists(join(version, 'VPinballX.ini'), false)) return join(version, 'VPinballX.ini');
  const legacy = join(vpx, 'VPinballX.ini');
  return await exists(legacy, false) ? legacy : null;
}
export async function resolveMaps(tables: string, source: string, vpx: string, override = ''): Promise<string> {
  if (override.trim()) return override;
  const table = dirname(join(tables, source));
  const local = await directory(join(table, 'pinmame/memmaps'));
  if (local) return local;
  const installation = await resolveVpx(vpx || await detectVpx());
  if (installation) {
    const ini = await findIni(installation);
    if (ini) {
      let section = '';
      for (const line of (await readFile(ini, 'utf8')).split(/\r?\n/)) {
        const text = line.trim();
        if (text.startsWith('[')) section = text.toLowerCase();
        if (section !== '[plugin.pinmame]' || /^[;#]/.test(text)) continue;
        const match = text.match(/^PinMAMEPath\s*=\s*(.*?)\s*$/i);
        if (match?.[1]) {
          const configured = await directory(join(match[1].replace(/^"|"$/g, ''), 'memmaps'));
          if (configured) return configured;
        }
      }
    }
    const plugins = process.platform === 'darwin' ? 'Contents/PlugIns' : 'plugins';
    const maps = await directory(join(installation, plugins, 'pinmame/memmaps'));
    if (maps) return maps;
  }
  throw new Error('PinMAME maps unavailable. Select your Visual Pinball installation in Folders.');
}
