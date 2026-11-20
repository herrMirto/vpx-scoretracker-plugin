#!/usr/bin/env python3
"""Archive the complete Electron application, including its bundled runtime."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import zipfile
ROOT = Path(__file__).resolve().parents[1]
PLATFORMS = {
    'macos-arm64': 'mac-arm64/ScoreTracker.app',
    'macos-x64': 'mac/ScoreTracker.app',
    'linux-x64': 'linux-unpacked',
    'linux-aarch64': 'linux-arm64-unpacked',
    'windows-x64': 'win-unpacked',
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', required=True, choices=PLATFORMS)
    args = parser.parse_args()
    version = (ROOT / 'VERSION').read_text(encoding='utf-8').strip()
    source = ROOT / 'build/viewer' / PLATFORMS[args.platform]
    if not source.is_dir():
        raise SystemExit('Build the desktop app first: bash viewer/build.sh')
    output = ROOT / 'dist' / f'scoretracker-viewer-{version}-{args.platform}.zip'
    output.parent.mkdir(exist_ok=True)
    if args.platform.startswith('macos-'):
        subprocess.run(['ditto', '-c', '-k', '--keepParent', str(source), str(output)], check=True)
    else:
        with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(source.rglob('*')):
                archive.write(path, Path('ScoreTracker') / path.relative_to(source))
    with zipfile.ZipFile(output, 'a', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('README.txt', 'ScoreTracker Desktop Viewer\n\nExtract the entire archive before opening ScoreTracker.\nThe runtime is bundled; no development tools are required.\nOn Linux, preserve executable permissions when extracting.\nInstall the separate plugin ZIP in the VPX plugins directory.\n')
        archive.write(ROOT / 'LICENSE', 'LICENSE.txt')
    output.with_suffix('.zip.sha256').write_text(f'{hashlib.sha256(output.read_bytes()).hexdigest()}  {output.name}\n', encoding='utf-8')
    print(output)

if __name__ == '__main__':
    main()
