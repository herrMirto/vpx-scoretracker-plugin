import { open, rename, rm } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { randomUUID } from 'node:crypto';
import koffi from 'koffi';

export async function atomicWrite(path: string, text: string): Promise<void> {
  const temp = join(dirname(path), `.${randomUUID()}.tmp`);
  try {
    const file = await open(temp, 'wx', 0o600);
    try { await file.writeFile(text); await file.sync(); }
    finally { await file.close(); }
    await rename(temp, path);
    if (process.platform !== 'win32') {
      const directory = await open(dirname(path), 'r');
      try { await directory.sync(); } finally { await directory.close(); }
    }
  } finally { await rm(temp, {force: true}); }
}

// Use the very same OS lock as the C++ plugin, rather than a separate JS lock
// file protocol. Nonblocking attempts keep the Electron main loop responsive.
let posixLock: ReturnType<ReturnType<typeof koffi.load>['func']> | undefined;
let windows: ReturnType<typeof windowsFunctions> | undefined;
function windowsFunctions() {
  const library = koffi.load('kernel32.dll');
  return {
    create: library.func('void * __stdcall CreateFileW(const char16_t *, uint32_t, uint32_t, void *, uint32_t, uint32_t, void *)'),
    lock: library.func('int __stdcall LockFileEx(void *, uint32_t, uint32_t, uint32_t, uint32_t, void *)'),
    close: library.func('int __stdcall CloseHandle(void *)'),
    error: library.func('uint32_t __stdcall GetLastError()'),
  };
}
export async function withHistoryLock<T>(directory: string, action: () => Promise<T>): Promise<T> {
  const path = join(directory, '.scoretracker.lock');
  const deadline = Date.now() + 10_000;
  let attempt: () => boolean;
  let release: () => Promise<void>;
  if (process.platform === 'win32') {
    const api = windows ??= windowsFunctions();
    const handle = api.create(path, 0xc0000000, 3, null, 4, 0x80, null);
    if (handle === null || handle === -1n || handle === 0xffffffffffffffffn) throw new Error('Cannot open history lock');
    const overlap = Buffer.alloc(32);
    attempt = () => {
      if (api.lock(handle, 3, 0, 1, 0, overlap)) return true;
      if (api.error() !== 33) throw new Error('Cannot lock history');
      return false;
    };
    release = async () => { api.close(handle); };
  } else {
    posixLock ??= koffi.load(process.platform === 'darwin' ? '/usr/lib/libSystem.B.dylib' : 'libc.so.6').func('int flock(int, int)');
    const file = await open(path, 'a+', 0o600);
    attempt = () => {
      if (posixLock!(file.fd, 2 | 4) === 0) return true;
      const errno = koffi.errno();
      if (errno !== 11 && errno !== 35) throw new Error(`Cannot lock history (${errno})`);
      return false;
    };
    release = async () => { posixLock!(file.fd, 8); await file.close(); };
  }
  try {
    while (!attempt()) {
      if (Date.now() >= deadline) throw new Error('History is busy; try again');
      await new Promise(resolve => setTimeout(resolve, 25));
    }
    return await action();
  } finally { await release(); }
}
