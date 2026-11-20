import type { ScanSnapshot } from './types';
import { readScanStream } from './scan-stream.ts';
export interface Configuration {
  tablesRoot: string; mapsRoot: string; vpxRoot: string;
  configurable: boolean; native: boolean; readOnly: boolean;
}
let token = '';
try {
  token = location.hash.match(/^#token=([a-f0-9]{64})$/)?.[1] ?? sessionStorage.getItem('scoretracker.session') ?? '';
  if (location.hash.startsWith('#token=')) { sessionStorage.setItem('scoretracker.session', token); history.replaceState(null, '', location.pathname); }
} catch { /* The URL token still works when browser storage is disabled. */ }
export async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
  const response = await fetch(path, { ...init, headers: { Accept: 'application/json', ...(token ? {'X-ScoreTracker-Token': token} : {}), ...init.headers } });
  if (!response.ok) throw new Error((await response.text()) || `Request failed (${response.status})`);
  return await response.json() as T;
}
export function post<T>(path: string, body?: unknown): Promise<T> {
  return request<T>(path, { method:'POST', headers: {'Content-Type':'application/json'}, body: body === undefined ? undefined : JSON.stringify(body) });
}
let scan: AbortController | null = null;
export function cancelScan(): void { scan?.abort(); }
export async function scanScores(onProgress: (snapshot: ScanSnapshot) => void): Promise<ScanSnapshot> {
  cancelScan();
  scan = new AbortController();
  const response = await fetch('/api/scores/stream', {signal:scan.signal,
    headers:{Accept:'application/x-ndjson', ...(token ? {'X-ScoreTracker-Token':token} : {})}});
  if (!response.ok) throw new Error((await response.text()) || `Request failed (${response.status})`);
  return readScanStream(response, onProgress);
}
export async function backend<T>(command: string, args: Record<string, unknown> = {}): Promise<T> {
  const query = new URLSearchParams();
  for (const [key, value] of Object.entries(args)) if (value !== null && value !== undefined) query.set(key, String(value));
  switch (command) {
    case 'resolve_vpx_hash': return request<T>(`/api/vpx-hash?${query}`);
    case 'load_nvram': return request<T>(`/api/nvram?${query}`);
    case 'remove_game': return post<T>('/api/remove-game', args);
    default: throw new Error(`Unknown Viewer action: ${command}`);
  }
}
