#!/bin/bash

set -euo pipefail

INSTALL_ROOT="/userdata/system/scoretracker"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SOURCE="$SCRIPT_DIR/pybrowser"
PORTS_LAUNCHER="$SCRIPT_DIR/ScoreTracker-Pybrowser.sh"

fail() {
    echo "ScoreTracker Pybrowser installation failed: $*" >&2
    exit 1
}

[ -d /userdata/system ] || fail "/userdata/system was not found; run this script on Batocera"
[ -x "$INSTALL_ROOT/bin/scoretracker-server" ] || fail "install ScoreTracker for Batocera first"
[ -f "$SOURCE/scoretracker_pybrowser.py" ] || fail "Pybrowser application is missing"
[ -f "$SOURCE/launch-pybrowser.sh" ] || fail "Pybrowser launcher is missing"
[ -f "$PORTS_LAUNCHER" ] || fail "Ports launcher is missing"
python3 -c "import pygame" >/dev/null 2>&1 || fail "Pygame is not available in Batocera's Python"

echo "Installing ScoreTracker Pybrowser..."
rm -rf "$INSTALL_ROOT/pybrowser.new"
cp -a "$SOURCE" "$INSTALL_ROOT/pybrowser.new"
rm -rf "$INSTALL_ROOT/pybrowser"
mv "$INSTALL_ROOT/pybrowser.new" "$INSTALL_ROOT/pybrowser"
chmod 755 "$INSTALL_ROOT/pybrowser/scoretracker_pybrowser.py" \
    "$INSTALL_ROOT/pybrowser/launch-pybrowser.sh"

mkdir -p /userdata/roms/ports
cp "$PORTS_LAUNCHER" /userdata/roms/ports/ScoreTracker.sh
chmod 755 /userdata/roms/ports/ScoreTracker.sh

batocera-services start ScoreTracker >/dev/null 2>&1 || true

echo
echo "ScoreTracker Pybrowser is installed."
echo "Refresh the EmulationStation game list, then open Ports > ScoreTracker."
