import type { ScanSnapshot } from './types';

// UTF-8 and JSON lines may both span network chunks. Require an explicit final
// message so a disconnected scan never presents partial history as complete.
export async function readScanStream(response: Response, onProgress: (snapshot: ScanSnapshot) => void): Promise<ScanSnapshot> {
  if (!response.body) throw new Error('The Viewer returned no score history');
  const reader = response.body.getReader();
  const decoder = new TextDecoder();
  let pending = '';
  try {
    while (true) {
      const {value, done} = await reader.read();
      pending += decoder.decode(value, {stream:!done});
      const lines = pending.split('\n');
      pending = lines.pop() ?? '';
      if (done && pending.trim()) { lines.push(pending); pending = ''; }
      for (const line of lines) {
        if (!line.trim()) continue;
        const message = JSON.parse(line) as {snapshot?:ScanSnapshot; complete?:boolean; error?:string};
        if (message.error) throw new Error(message.error);
        if (!message.snapshot) throw new Error('Invalid score history response');
        if (message.complete) return message.snapshot;
        onProgress(message.snapshot);
      }
      if (done) throw new Error('Score reading was interrupted. Refresh to try again.');
    }
  } finally { await reader.cancel().catch(() => {}); reader.releaseLock(); }
}
