// Embed the one UI in a single HTML asset for both desktop and service builds.
import { readFile, writeFile, rm } from 'node:fs/promises';
const root = new URL('../dist/', import.meta.url);
let html = await readFile(new URL('index.html', root), 'utf8');
const js = html.match(/<script\b[^>]*src="([^"]+)"[^>]*><\/script>/);
const css = html.match(/<link\b[^>]*href="([^"]+\.css)"[^>]*>/);
if (!js || !css) throw new Error('The UI build did not produce its script and stylesheet');
const code = await readFile(new URL(js[1].replace(/^\//,''), root), 'utf8');
const style = await readFile(new URL(css[1].replace(/^\//,''), root), 'utf8');
html = html.replace(js[0], '').replace(css[0], () => `<style>${style}</style>`).replace('</body>', () => `<script type="module">${code.replace(/<\/script/gi, '<\\/script')}</script></body>`);
await writeFile(new URL('index.html', root), html);

await rm(new URL("assets/", root), {recursive:true, force:true});
