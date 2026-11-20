import { homedir } from 'node:os';
import { join } from 'node:path';
export function argument(name: string): string | undefined {
  const index = process.argv.indexOf(name);
  return index < 0 ? undefined : process.argv[index + 1];
}
export function dataDirectory(): string {
  const base = process.platform === 'darwin' ? join(homedir(), 'Library/Application Support')
    : process.platform === 'win32' ? process.env.LOCALAPPDATA ?? join(homedir(), 'AppData/Local')
      : process.env.XDG_DATA_HOME ?? join(homedir(), '.local/share');
  return argument('--data-dir') ?? join(base, 'ScoreTracker');
}
