#!/usr/bin/env python3
"""Check local source reachability and package references without network or builds."""
import ast
import json
import re
import subprocess
import sys
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
errors = []
checked = 0


def require(path, owner):
    global checked
    checked += 1
    if not path.is_file():
        errors.append(f'{owner}: missing file {path.relative_to(ROOT)}')


def frontend_graph(entries):
    seen = set()
    pending = list(entries)
    while pending:
        path = pending.pop().resolve()
        if path in seen or not path.is_file():
            continue
        seen.add(path)
        if path.suffix not in ('.ts', '.mjs', '.js'):
            continue
        for reference in re.findall(r'(?:from\s*|import\s*)[\'\"]([^\'\"]+)[\'\"]', path.read_text(encoding='utf-8')):
            if not reference.startswith('.'):
                continue  # Node built-ins and declared npm dependencies are checked by tsc/build.
            target = path.parent / reference
            if not target.suffix:
                target = target.with_suffix('.ts')
            require(target, path.relative_to(ROOT))
            pending.append(target)
    return seen


frontend = ROOT / 'companion'
reachable = frontend_graph([frontend / 'src/main.ts'])
for path in (frontend / 'src').rglob('*.ts'):
    if path.resolve() not in reachable:
        errors.append(f'Frontend source is disconnected from main.ts: {path.relative_to(ROOT)}')
frontend_graph(list((frontend / 'tests').glob('*.ts')))
package = json.loads((frontend / 'package.json').read_text(encoding='utf-8'))
for command in package['scripts'].values():
    for reference in re.findall(r'node\s+((?:scripts|tests)/[\w.-]+)', command):
        require(frontend / reference, 'companion/package.json')

desktop = frontend_graph([frontend / 'desktop/main.ts', frontend / 'desktop/headless.ts'])
for path in (frontend / 'desktop').rglob('*.ts'):
    if path.resolve() not in desktop:
        errors.append(f'Desktop source is disconnected from its entry points: {path.relative_to(ROOT)}')
if (frontend / 'desktop-dist').exists():
    require(frontend / package['main'], 'Electron entry point')


cmake = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
manifest_files = set(re.findall(r'((?:src|third-party)/[\w./-]+\.(?:cpp|c|h))(?![\w])', cmake))
for name in manifest_files:
    if name.startswith('third-party/include/'):
        continue
    require(ROOT / name, 'CMake plugin manifest')
for pattern in ('*.cpp', '*.h'):
    for path in (ROOT / 'src').glob(pattern):
        if 'src/' + path.name not in manifest_files:
            errors.append(f'C++ source/header is absent from the build manifest: {path.name}')


class LocalReferences(HTMLParser):
    def __init__(self, path):
        super().__init__()
        self.path = path

    def handle_starttag(self, tag, attrs):
        for name, value in attrs:
            if name not in ('src', 'href') or not value:
                continue
            url = urlsplit(value)
            if url.scheme or url.netloc or not url.path:
                continue
            base = frontend if self.path.parent == frontend else self.path.parent
            target = base / unquote(url.path).lstrip('/')
            require(target, self.path.relative_to(ROOT))


html_entries = [frontend / 'index.html']
if (ROOT / 'pages/index.html').is_file():
    html_entries.append(ROOT / 'pages/index.html')
for path in html_entries:
    LocalReferences(path).feed(path.read_text(encoding='utf-8'))
# Build helpers and local workflow actions must exist even on platforms not tested here.
for path in (ROOT / '.github/workflows').glob('*.yml'):
    text = path.read_text(encoding='utf-8')
    for reference in re.findall(r'uses:\s*\./([^\s]+)', text):
        require(ROOT / reference, path.relative_to(ROOT))
    for reference in re.findall(r'\b(?:python3|bash)\s+((?:scripts|tests|viewer|\.github)/[\w./-]+\.(?:py|sh))', text):
        require(ROOT / reference, path.relative_to(ROOT))
    for retired in ('companion/src-tauri', 'installer/src', 'batocera/server', 'assets/icon.png'):
        if retired in text:
            errors.append(f'{path.relative_to(ROOT)} still references retired {retired}')

# Markdown references are documentation entry points, not runtime imports.
for directory in (ROOT, frontend, ROOT / 'viewer', ROOT / 'docs'):
    for path in directory.glob('*.md'):
        for reference in re.findall(r'\[[^\]]*\]\(([^)]+)\)', path.read_text(encoding='utf-8')):
            url = urlsplit(reference)
            if url.scheme or url.netloc or not url.path:
                continue
            target = path.parent / unquote(url.path)
            require(target, path.relative_to(ROOT))

for directory in ('scripts', 'viewer', 'tests'):
    for path in (ROOT / directory).rglob('*.py'):
        ast.parse(path.read_text(encoding='utf-8'), filename=str(path))
    for path in (ROOT / directory).rglob('*.sh'):
        subprocess.run(['bash', '-n', str(path)], check=True)
for path in (ROOT / 'build-standalone.sh',):
    subprocess.run(['bash', '-n', str(path)], check=True)
for path in list((frontend / 'scripts').glob('*.mjs')) + list((ROOT / 'pages').glob('*.js')):
    subprocess.run(['node', '--check', str(path)], check=True)
subprocess.run([sys.executable, str(ROOT / 'scripts/check-version.py')], check=True)
if errors:
    print('\n'.join(f'ERROR: {error}' for error in errors), file=sys.stderr)
    raise SystemExit(1)
print(f'PASS: {checked} file references; TypeScript reachability; C++ manifest; HTML assets; workflow helpers; Python/shell/JavaScript syntax')
