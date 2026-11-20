#!/usr/bin/env python3
"""Package the staged plugin folder for extraction directly into VPX plugins/."""
import argparse
import configparser
import hashlib
from pathlib import Path
import zipfile
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--platform',required=True,choices=['macos-arm64','macos-x64','linux-x64','linux-aarch64','windows-x64','windows-x86','windows-arm64','windows-mingw-x64'])
parser.add_argument('--build-dir',type=Path,default=ROOT/'build')
args=parser.parse_args()
stage=args.build_dir/'scoretracker'
config=configparser.ConfigParser();config.read(stage/'plugin.cfg')
platform,arch=args.platform.split('-',1)
if platform=='windows' and arch=='mingw-x64':arch='x64'
module=config['libraries'][f'{platform}.{arch}'].strip('"')
if not (stage/module).is_file():raise SystemExit(f'Missing staged plugin: {module}')
version=(ROOT/'VERSION').read_text().strip()
output=ROOT/'dist'/f'scoretracker-plugin-{version}-{args.platform}.zip'
output.parent.mkdir(exist_ok=True)
# Allowlist: user histories, preferences and private keys must never enter a release.
files=[stage/'plugin.cfg',stage/module,stage/'README.txt',stage/'CAPTURE-CONTRACT.md',*sorted((stage/'licenses').glob('*')),*sorted((stage/'schema').glob('*.json')),*sorted((stage/'examples').glob('*'))]
revisions=ROOT/'third-party/include/revisions.txt'
with zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED) as archive:
 for file in files:
  if not file.is_file() or file.is_symlink():raise SystemExit(f'Invalid package file: {file}')
  archive.write(file,Path('scoretracker')/file.relative_to(stage))
 if revisions.exists():archive.write(revisions,'scoretracker/sdk-revisions.txt')
checksum=hashlib.sha256(output.read_bytes()).hexdigest()
output.with_suffix('.zip.sha256').write_text(f'{checksum}  {output.name}\n')
print(output)
