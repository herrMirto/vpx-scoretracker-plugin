#!/usr/bin/env python3

import argparse
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def replace_pattern(path: str, pattern: str, replacement: str, expected: int = 1) -> None:
    file_path = ROOT / path
    content = file_path.read_text(encoding="utf-8")
    updated, count = re.subn(pattern, replacement, content, flags=re.MULTILINE)
    if count != expected:
        raise SystemExit(f"{path}: expected {expected} version match(es), found {count}")
    file_path.write_text(updated, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="Synchronize the ScoreTracker patch version.")
    parser.add_argument("--dry-run", action="store_true", help="Print the next version without editing files.")
    args = parser.parse_args()

    current = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", current)
    if match is None:
        raise SystemExit(f"VERSION is not a numeric semantic version: {current}")

    major, minor, patch = (int(part) for part in match.groups())
    next_version = f"{major}.{minor}.{patch + 1}"
    if args.dry_run:
        print(next_version)
        return

    replace_pattern("VERSION", rf"^{re.escape(current)}$", next_version)
    replace_pattern(
        "plugin.cfg",
        rf'(^version = "){re.escape(current)}("$)',
        rf"\g<1>{next_version}\g<2>",
    )
    replace_pattern(
        "installer/Cargo.toml",
        rf'(name = "scoretracker-installer"\nversion = "){re.escape(current)}(")',
        rf"\g<1>{next_version}\g<2>",
    )
    replace_pattern(
        "installer/Cargo.lock",
        rf'(name = "scoretracker-installer"\nversion = "){re.escape(current)}(")',
        rf"\g<1>{next_version}\g<2>",
    )
    replace_pattern(
        "companion/package.json",
        rf'("name": "vpx-scoretracker-viewer",\n  "private": true,\n  "version": "){re.escape(current)}(")',
        rf"\g<1>{next_version}\g<2>",
    )
    replace_pattern(
        "companion/package-lock.json",
        rf'("name": "vpx-scoretracker-viewer",\n\s+"version": "){re.escape(current)}(")',
        rf"\g<1>{next_version}\g<2>",
        expected=2,
    )
    replace_pattern(
        "companion/src-tauri/Cargo.toml",
        rf'(name = "vpx-scoretracker-viewer"\nversion = "){re.escape(current)}(")',
        rf"\g<1>{next_version}\g<2>",
    )
    replace_pattern(
        "companion/src-tauri/Cargo.lock",
        rf'(name = "vpx-scoretracker-viewer"\nversion = "){re.escape(current)}(")',
        rf"\g<1>{next_version}\g<2>",
    )
    replace_pattern(
        "companion/src-tauri/tauri.conf.json",
        rf'("productName": "VPX Scoretracker Viewer",\n  "version": "){re.escape(current)}(")',
        rf"\g<1>{next_version}\g<2>",
    )

    subprocess.run(["python3", str(ROOT / "scripts/check-version.py")], check=True)
    print(f"Bumped ScoreTracker {current} -> {next_version}")


if __name__ == "__main__":
    main()
