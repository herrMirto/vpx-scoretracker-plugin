#!/bin/bash

set -u

INSTALL_ROOT="/userdata/system/scoretracker"
APP="$INSTALL_ROOT/pybrowser/scoretracker_pybrowser.py"
LOG="$INSTALL_ROOT/pybrowser.log"
CONFIG_PATH="$INSTALL_ROOT/scoretracker.conf"

if [ ! -f "$APP" ]; then
    echo "ScoreTracker Pybrowser was not found: $APP" >>"$LOG"
    exit 1
fi

listen_port=8080
if [ -f "$CONFIG_PATH" ]; then
    configured_port="$(sed -n 's/^listen_port=//p' "$CONFIG_PATH" | tail -n 1)"
    case "$configured_port" in
        ''|*[!0-9]*) ;;
        *)
            if [ "$configured_port" -ge 1 ] && [ "$configured_port" -le 65535 ]; then
                listen_port="$configured_port"
            fi
            ;;
    esac
fi

export SCORETRACKER_API_URL="${SCORETRACKER_API_URL:-http://127.0.0.1:$listen_port}"
exec python3 "$APP" >>"$LOG" 2>&1
