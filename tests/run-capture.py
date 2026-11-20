#!/usr/bin/env python3
"""Run deterministic capture/storage tests; no ROMs, delays or installed app changes."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
flags = ['-std=c++20', '-pthread', '-g', '-Wall', '-Wextra', '-Wno-unknown-pragmas', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-I'+str(root/'src'), '-I'+str(root/'third-party/include')]
if args.sanitize:
    flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
with tempfile.TemporaryDirectory(prefix='scoretracker-capture-') as temp:
    work = Path(temp)
    for name, sources in [
        ('capture', ['tests/capture.cpp', 'src/GameRecord.cpp', 'src/ScoresFileWriter.cpp', 'src/ScriptRecorder.cpp', 'src/PinMameCapture.cpp']),
    ]:
        binary = work/name
        subprocess.run(['c++', *flags, *[str(root/s) for s in sources], '-o', str(binary)], check=True)
        subprocess.run([str(binary), str(work/'history')], check=True)
    concurrent = work/'concurrent'
    processes = [subprocess.Popen([str(work/'capture'), str(concurrent), 'append', f'{i:032x}']) for i in range(1, 13)]
    assert all(p.wait() == 0 for p in processes)
    document = json.loads((concurrent/'scores.json').read_text())
    assert len(document['games']) == 12 and len({g['id'] for g in document['games']}) == 12
    assert not list((concurrent/'.scoretracker-pending').glob('*.json'))
    print('PASS: 12 concurrent processes, no lost records or shared temporary files')
