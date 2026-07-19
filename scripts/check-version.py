#!/usr/bin/env python3

import configparser
import json
import sys
import tomllib
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
expected = (ROOT / "VERSION").read_text(encoding="utf-8").strip()

plugin = configparser.ConfigParser()
plugin.read(ROOT / "plugin.cfg", encoding="utf-8")

with (ROOT / "installer/Cargo.toml").open("rb") as file:
    installer = tomllib.load(file)
with (ROOT / "companion/src-tauri/Cargo.toml").open("rb") as file:
    viewer = tomllib.load(file)

package = json.loads((ROOT / "companion/package.json").read_text(encoding="utf-8"))
package_lock = json.loads((ROOT / "companion/package-lock.json").read_text(encoding="utf-8"))
tauri = json.loads((ROOT / "companion/src-tauri/tauri.conf.json").read_text(encoding="utf-8"))

versions = {
    "plugin.cfg": plugin["configuration"]["version"].strip("\"'"),
    "installer/Cargo.toml": installer["package"]["version"],
    "companion/package.json": package["version"],
    "companion/package-lock.json": package_lock["version"],
    "companion/package-lock.json root package": package_lock["packages"][""]["version"],
    "companion/src-tauri/Cargo.toml": viewer["package"]["version"],
    "companion/src-tauri/tauri.conf.json": tauri["version"],
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
