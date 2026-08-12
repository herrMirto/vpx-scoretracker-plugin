#!/bin/bash

set -euo pipefail

INSTALL_ROOT="/userdata/system/scoretracker"
SERVICE_NAME="ScoreTracker"
SERVICE_PATH="/userdata/system/services/$SERVICE_NAME"
VPX_INI="/userdata/system/configs/vpinball/VPinballX.ini"
PINMAME_ROOT="/userdata/system/configs/vpinball/pinmame"
CONFIG_PATH="$INSTALL_ROOT/scoretracker.conf"
PORTS_ROOT="/userdata/roms/ports"
ES_SERVICE="/etc/init.d/S31emulationstation"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PAYLOAD="$SCRIPT_DIR/payload"

fail() {
    echo "ScoreTracker installation failed: $*" >&2
    exit 1
}

requested_port=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --listen-port)
            [ "$#" -ge 2 ] || fail "--listen-port requires a port number"
            requested_port="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [--listen-port PORT]"
            exit 0
            ;;
        *)
            fail "unknown option: $1"
            ;;
    esac
done

saved_port=""
if [ -f "$CONFIG_PATH" ]; then
    saved_port="$(sed -n 's/^listen_port=//p' "$CONFIG_PATH" | tail -n 1)"
fi
listen_port="${requested_port:-${saved_port:-8080}}"
case "$listen_port" in
    ''|*[!0-9]*) fail "listen port must be a number between 1 and 65535" ;;
esac
[ "$listen_port" -ge 1 ] && [ "$listen_port" -le 65535 ] \
    || fail "listen port must be between 1 and 65535"

[ "$(uname -m)" = "x86_64" ] || fail "this preview supports Batocera x86-64 only"
[ -d /userdata/system ] || fail "/userdata/system was not found; run this script on Batocera"
[ -d /usr/bin/vpinball/plugins ] || fail "the installed VPX build has no plugin directory; install a current plugin-enabled VPX build first"
[ -x "$PAYLOAD/bin/scoretracker-server" ] || fail "server payload is missing"
[ -f "$PAYLOAD/plugin/plugin.cfg" ] || fail "plugin payload is missing"
[ -f "$PAYLOAD/plugin/maps/index.json" ] || fail "NVRAM maps payload is missing"
[ -f "$PAYLOAD/web/index.html" ] || fail "web dashboard payload is missing"
[ -f "$PAYLOAD/pybrowser/scoretracker_pybrowser.py" ] || fail "Pybrowser payload is missing"
[ -f "$PAYLOAD/pybrowser/launch-pybrowser.sh" ] || fail "Pybrowser launcher payload is missing"
[ -f "$SCRIPT_DIR/ScoreTracker" ] || fail "Batocera service script is missing"
[ -f "$SCRIPT_DIR/ScoreTracker-Pybrowser.sh" ] || fail "Batocera Ports launcher is missing"
[ -f "$SCRIPT_DIR/update-scoretracker.sh" ] || fail "Batocera updater is missing"
[ -f "$SCRIPT_DIR/update-ports-gamelist.py" ] || fail "Batocera gamelist helper is missing"
[ -f "$SCRIPT_DIR/ScoreTracker.png" ] || fail "Batocera Ports icon is missing"
[ -f "$SCRIPT_DIR/build-info.json" ] || fail "package build information is missing"

echo "Installing ScoreTracker for Batocera..."
mkdir -p "$INSTALL_ROOT" /userdata/system/services

existing_service=0
if [ -f "$SERVICE_PATH" ]; then
    existing_service=1
    batocera-services stop "$SERVICE_NAME" >/dev/null 2>&1 || true
fi

if ! python3 -c 'import socket, sys; s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("0.0.0.0", int(sys.argv[1]))); s.close()' "$listen_port" 2>/dev/null; then
    if [ "$existing_service" -eq 1 ]; then
        batocera-services start "$SERVICE_NAME" >/dev/null 2>&1 || true
    fi
    fail "port $listen_port is already in use; choose another with --listen-port PORT"
fi

for component in bin plugin web pybrowser; do
    rm -rf "$INSTALL_ROOT/$component.new"
    cp -a "$PAYLOAD/$component" "$INSTALL_ROOT/$component.new"
    rm -rf "$INSTALL_ROOT/$component"
    mv "$INSTALL_ROOT/$component.new" "$INSTALL_ROOT/$component"
done
mkdir -p "$PINMAME_ROOT/memmaps"
cp -a "$INSTALL_ROOT/plugin/maps/." "$PINMAME_ROOT/memmaps/"
chmod 755 "$INSTALL_ROOT/bin/scoretracker-server"
chmod 755 "$INSTALL_ROOT/pybrowser/scoretracker_pybrowser.py" \
    "$INSTALL_ROOT/pybrowser/launch-pybrowser.sh"
cp "$SCRIPT_DIR/ScoreTracker" "$SERVICE_PATH"
chmod 755 "$SERVICE_PATH"
cp "$SCRIPT_DIR/update-scoretracker.sh" "$INSTALL_ROOT/update-scoretracker.sh"
chmod 755 "$INSTALL_ROOT/update-scoretracker.sh"
cp "$SCRIPT_DIR/build-info.json" "$INSTALL_ROOT/build-info.json"
printf 'listen_port=%s\n' "$listen_port" > "$CONFIG_PATH.new"
mv "$CONFIG_PATH.new" "$CONFIG_PATH"

mkdir -p "$PORTS_ROOT/images"
cp "$SCRIPT_DIR/ScoreTracker-Pybrowser.sh" "$PORTS_ROOT/ScoreTracker.sh"
chmod 755 "$PORTS_ROOT/ScoreTracker.sh"
cp "$SCRIPT_DIR/ScoreTracker.png" "$PORTS_ROOT/images/ScoreTracker.png"

es_was_running=0
if [ -x "$ES_SERVICE" ] && command -v batocera-es-swissknife >/dev/null 2>&1 \
    && batocera-es-swissknife --espid >/dev/null 2>&1; then
    es_was_running=1
    "$ES_SERVICE" stop >/dev/null 2>&1 || true
fi
if ! python3 "$SCRIPT_DIR/update-ports-gamelist.py" "$PORTS_ROOT/gamelist.xml"; then
    echo "Warning: the Ports icon was installed, but gamelist.xml could not be updated." >&2
fi
if [ "$es_was_running" -eq 1 ]; then
    "$ES_SERVICE" start >/dev/null 2>&1 &
fi

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

awk -v pinmame_path="$PINMAME_ROOT" '
BEGIN { in_section = 0; section_seen = 0; path_seen = 0 }
/^[[:space:]]*\[/ {
    if (in_section && !path_seen) print "PinMAMEPath = " pinmame_path
    in_section = ($0 ~ /^[[:space:]]*\[Plugin\.PinMAME\]/)
    if (in_section) { section_seen = 1; path_seen = 0 }
}
in_section && tolower($0) ~ /^[[:space:]]*pinmamepath[[:space:]]*=/ {
    if (!path_seen) print "PinMAMEPath = " pinmame_path
    path_seen = 1
    next
}
{ print }
END {
    if (in_section && !path_seen) print "PinMAMEPath = " pinmame_path
    if (!section_seen) {
        print ""
        print "[Plugin.PinMAME]"
        print "PinMAMEPath = " pinmame_path
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
echo "Open this address on another device: http://$address:$listen_port"
echo "Native viewer: update the EmulationStation game list, then open Ports > VPX ScoreTracker"
echo "Health check: http://$address:$listen_port/api/health"
echo "Log file: $INSTALL_ROOT/scoretracker-server.log"
echo
echo "The service reapplies the VPX plugin at every boot because Batocera's /usr filesystem is not persistent."
