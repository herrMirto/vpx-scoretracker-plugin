#!/usr/bin/env python3

import configparser
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
expected = (ROOT / "VERSION").read_text(encoding="utf-8").strip()

plugin = configparser.ConfigParser()
plugin.read(ROOT / "src/plugin.cfg", encoding="utf-8")

package = json.loads((ROOT / "companion/package.json").read_text(encoding="utf-8"))
package_lock = json.loads((ROOT / "companion/package-lock.json").read_text(encoding="utf-8"))

versions = {
    "src/plugin.cfg": plugin["configuration"]["version"].strip("\"'"),
    "companion/package.json": package["version"],
    "companion/package-lock.json": package_lock["version"],
    "companion/package-lock.json root package": package_lock["packages"][""]["version"],
}

errors = [
    f"{path}: expected {expected}, found {version}"
    for path, version in versions.items()
    if version != expected
]

if len(sys.argv) == 3 and sys.argv[1] == "--tag":
    tag_version = sys.argv[2].removeprefix("v")
    if tag_version != expected:
        errors.append(f"release tag: expected v{expected}, found {sys.argv[2]}")

if errors:
    print("Version mismatch:")
    print("\n".join(f"- {error}" for error in errors))
    raise SystemExit(1)

print(f"All ScoreTracker package versions are {expected}.")
