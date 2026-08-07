#!/bin/bash

set -u

INSTALL_ROOT="/userdata/system/scoretracker"
APP="$INSTALL_ROOT/pybrowser/scoretracker_pybrowser.py"
LOG="$INSTALL_ROOT/pybrowser.log"

if [ ! -f "$APP" ]; then
    echo "ScoreTracker Pybrowser was not found: $APP" >>"$LOG"
    exit 1
fi

export SCORETRACKER_API_URL="${SCORETRACKER_API_URL:-http://127.0.0.1:8080}"
exec python3 "$APP" >>"$LOG" 2>&1
