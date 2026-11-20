#!/usr/bin/env bash
# Build without installing into VPX, then test the real module and capture contract.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${1:-}" ]]; then
  export VPINBALL_SOURCE_DIR="$1"
fi
bash "$ROOT/build-standalone.sh" --no-install
python3 "$ROOT/tests/run-capture.py"
if [[ "$(uname -s)" == Linux ]]; then export SCORETRACKER_TEST_LINK_FLAGS="${SCORETRACKER_TEST_LINK_FLAGS:--ldl}"; fi
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
c++ -std=c++20 -I"$ROOT/third-party/include" \
  "$ROOT/tests/compatibility.cpp" -o "$WORK/compatibility" ${SCORETRACKER_TEST_LINK_FLAGS:-}
MODULE="$ROOT/build/scoretracker/plugin-scoretracker.so"
if [[ "$(uname -s)" == Darwin ]]; then MODULE="$ROOT/build/scoretracker/plugin-scoretracker.dylib"; fi
"$WORK/compatibility" "$MODULE" "$WORK/history"
