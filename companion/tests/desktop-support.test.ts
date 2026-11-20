import assert from 'node:assert/strict';
import test from 'node:test';
import { bounded, closeDesktop } from './desktop-support.mjs';

test('desktop deadlines preserve results and original errors', async () => {
  assert.equal(await bounded(async () => 42, 100, 'fixture'), 42);
  const original = new Error('renderer failed');
  await assert.rejects(bounded(async () => { throw original; }, 100, 'fixture'), error => error === original);
});

test('a stalled desktop operation reports its own stage', async () => {
  await assert.rejects(bounded(() => new Promise(() => {}), 10, 'Configuration API'), /Configuration API timed out/);
});

test('a stalled Electron shutdown kills the test app instead of hiding an earlier failure indefinitely', async () => {
  let signal: string | undefined;
  const application = {close: () => new Promise(() => {}), process: () => ({kill(value: string) { signal = value; }})};
  await assert.rejects(closeDesktop(application, 10), /Electron shutdown timed out/);
  assert.equal(signal, 'SIGKILL');
});

test('normal Electron shutdown does not force-kill the app', async () => {
  await closeDesktop({close: async () => {}, process() { throw new Error('Unexpected kill'); }}, 100);
});
