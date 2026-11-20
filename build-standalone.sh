#!/usr/bin/env bash
# Convenience wrapper. Never installs or changes a VPX checkout.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
[[ "${1:-}" != --no-install ]] || shift
if [[ $# -gt 0 ]]; then
  export VPINBALL_SOURCE_DIR="$1"
  echo "Local SDK override: $VPINBALL_SOURCE_DIR"
fi
bash "$ROOT/platforms/stage-dependencies.sh"
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build" --parallel
if [[ "$(uname -s)" == Darwin ]]; then
  codesign --force --sign - "$ROOT/build/scoretracker/plugin-scoretracker.dylib"
fi
