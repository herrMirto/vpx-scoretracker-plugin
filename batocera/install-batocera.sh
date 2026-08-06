#!/bin/bash

set -euo pipefail

INSTALL_ROOT="/userdata/system/scoretracker"
SERVICE_NAME="ScoreTracker"
SERVICE_PATH="/userdata/system/services/$SERVICE_NAME"
VPX_INI="/userdata/system/configs/vpinball/VPinballX.ini"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PAYLOAD="$SCRIPT_DIR/payload"

fail() {
    echo "ScoreTracker installation failed: $*" >&2
    exit 1
}

[ "$(uname -m)" = "x86_64" ] || fail "this preview supports Batocera x86-64 only"
[ -d /userdata/system ] || fail "/userdata/system was not found; run this script on Batocera"
[ -d /usr/bin/vpinball/plugins ] || fail "the installed VPX build has no plugin directory; install a current plugin-enabled VPX build first"
[ -x "$PAYLOAD/bin/scoretracker-server" ] || fail "server payload is missing"
[ -f "$PAYLOAD/plugin/plugin.cfg" ] || fail "plugin payload is missing"
[ -f "$PAYLOAD/plugin/maps/index.json" ] || fail "NVRAM maps payload is missing"
[ -f "$PAYLOAD/web/index.html" ] || fail "web dashboard payload is missing"
[ -f "$SCRIPT_DIR/ScoreTracker" ] || fail "Batocera service script is missing"

echo "Installing ScoreTracker for Batocera..."
mkdir -p "$INSTALL_ROOT" /userdata/system/services

if [ -f "$SERVICE_PATH" ]; then
    batocera-services stop "$SERVICE_NAME" >/dev/null 2>&1 || true
fi

for component in bin plugin web; do
    rm -rf "$INSTALL_ROOT/$component.new"
    cp -a "$PAYLOAD/$component" "$INSTALL_ROOT/$component.new"
    rm -rf "$INSTALL_ROOT/$component"
    mv "$INSTALL_ROOT/$component.new" "$INSTALL_ROOT/$component"
done
chmod 755 "$INSTALL_ROOT/bin/scoretracker-server"
cp "$SCRIPT_DIR/ScoreTracker" "$SERVICE_PATH"
chmod 755 "$SERVICE_PATH"

mkdir -p "$(dirname "$VPX_INI")"
touch "$VPX_INI"
if [ ! -f "$VPX_INI.scoretracker-backup" ]; then
    cp "$VPX_INI" "$VPX_INI.scoretracker-backup"
fi

tmp_ini="$VPX_INI.scoretracker-tmp"
awk '
BEGIN { in_section = 0; section_seen = 0; enable_seen = 0 }
/^[[:space:]]*\[/ {
    if (in_section && !enable_seen) print "Enable = 1"
    in_section = ($0 ~ /^[[:space:]]*\[Plugin\.ScoreTracker\]/)
    if (in_section) { section_seen = 1; enable_seen = 0 }
}
in_section && tolower($0) ~ /^[[:space:]]*enable[[:space:]]*=/ {
    if (!enable_seen) print "Enable = 1"
    enable_seen = 1
    next
}
{ print }
END {
    if (in_section && !enable_seen) print "Enable = 1"
    if (!section_seen) {
        print ""
        print "[Plugin.ScoreTracker]"
        print "Enable = 1"
    }
}
' "$VPX_INI" > "$tmp_ini"
mv "$tmp_ini" "$VPX_INI"

batocera-services enable "$SERVICE_NAME"
batocera-services start "$SERVICE_NAME"

address="$(hostname -I 2>/dev/null | awk '{print $1}')"
[ -n "$address" ] || address="batocera.local"

echo
echo "ScoreTracker is installed and running."
echo "Open this address on another device: http://$address:8080"
echo "Health check: http://$address:8080/api/health"
echo "Log file: $INSTALL_ROOT/scoretracker-server.log"
echo
echo "The service reapplies the VPX plugin at every boot because Batocera's /usr filesystem is not persistent."
