import test from 'node:test';
import { loadMap } from '../desktop/nvram.ts';
test('representative upstream memory maps load', {skip: !process.env.SCORETRACKER_MAPS_ROOT}, async () => {
  for (const rom of ['taf_l7', 'afm_113', 'fh_l9', 't2_l8', 'acd_170']) await loadMap(process.env.SCORETRACKER_MAPS_ROOT!, rom);
});
