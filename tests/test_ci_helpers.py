"""Regression checks for CI helpers that must not depend on Windows' Bash lookup."""
import contextlib
import io
import hashlib
import os
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


class ValidatorTests(unittest.TestCase):
    def test_validation_uses_utf8_and_never_launches_bash(self):
        read_text = Path.read_text
        run = subprocess.run
        commands = []

        def windows_read(path, *args, **kwargs):
            # Python 3.12 on Windows normally defaults to the ANSI code page.
            if not args and kwargs.get('encoding') is None:
                kwargs['encoding'] = 'cp1252'
            return read_text(path, *args, **kwargs)

        def checked_run(command, *args, **kwargs):
            executable = Path(command[0]).name.lower()
            self.assertNotIn(executable, ('bash', 'bash.exe', 'wsl', 'wsl.exe'))
            commands.append(command)
            return run(command, *args, **kwargs)

        output = io.StringIO()
        with patch.object(Path, 'read_text', windows_read), patch.object(subprocess, 'run', checked_run):
            with contextlib.redirect_stdout(output):
                runpy.run_path(str(ROOT / 'scripts/validate-project.py'), run_name='__main__')
        self.assertIn('PASS:', output.getvalue())
        self.assertTrue(commands)  # JavaScript and version checks still execute.


class PackagingTests(unittest.TestCase):
    def test_windows_plugin_and_viewer_archive_layout(self):
        # Packaging fixtures only; this does not claim to build Windows binaries.
        with tempfile.TemporaryDirectory(prefix='scoretracker-packaging-') as temporary:
            root = Path(temporary) / 'checkout with spaces'
            scripts = root / 'scripts'
            scripts.mkdir(parents=True)
            for name in ('package-plugin.py', 'package-viewer.py'):
                shutil.copy2(ROOT / 'scripts' / name, scripts / name)
            (root / 'VERSION').write_text('0.1.23\n', encoding='utf-8')
            (root / 'LICENSE').write_text('Test license\n', encoding='utf-8')
            stage = root / 'build/scoretracker'
            stage.mkdir(parents=True)
            (stage / 'plugin.cfg').write_text('[configuration]\nauthor = 日本\n[libraries]\nwindows.x64 = "plugin-scoretracker64.dll"\n', encoding='utf-8')
            for name in ('plugin-scoretracker64.dll', 'README.txt', 'CAPTURE-CONTRACT.md'):
                (stage / name).write_bytes(b'fixture')
            (stage / 'scores.json').write_text('private history', encoding='utf-8')
            viewer = root / 'build/viewer/win-unpacked'
            (viewer / 'resources').mkdir(parents=True)
            (viewer / 'ScoreTracker.exe').write_bytes(b'fixture')
            (viewer / 'resources/app.asar').write_bytes(b'fixture')
            for kind in ('plugin', 'viewer'):
                result = subprocess.run([sys.executable, str(scripts / f'package-{kind}.py'), '--platform', 'windows-x64'],
                    check=True, capture_output=True, text=True, encoding='utf-8', env={**os.environ, 'PYTHONUTF8': '1'})
                archive = root / f'dist/scoretracker-{kind}-0.1.23-windows-x64.zip'
                self.assertTrue(archive.is_file(), result.stdout)
                checksum = archive.with_suffix('.zip.sha256').read_text(encoding='utf-8').split()[0]
                self.assertEqual(checksum, hashlib.sha256(archive.read_bytes()).hexdigest())
                with zipfile.ZipFile(archive) as zipped:
                    self.assertIsNone(zipped.testzip())
                    names = zipped.namelist()
                    self.assertTrue(all('\\' not in name for name in names))
                    if kind == 'plugin':
                        self.assertIn('scoretracker/plugin-scoretracker64.dll', names)
                        self.assertNotIn('scoretracker/scores.json', names)
                        self.assertIn('日本', zipped.read('scoretracker/plugin.cfg').decode('utf-8'))
                    else:
                        self.assertIn('ScoreTracker/ScoreTracker.exe', names)
                        self.assertIn('ScoreTracker/resources/app.asar', names)


if __name__ == '__main__':
    unittest.main()
