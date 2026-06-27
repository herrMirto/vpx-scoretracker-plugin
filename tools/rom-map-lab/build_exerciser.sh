#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VPINBALL_ROOT="${VPINBALL_ROOT:-/Users/andremichi/vpinball}"
PINMAME_SRC="${PINMAME_SRC:-$VPINBALL_ROOT/external/macos-arm64/Release/pinmame/pinmame/src/libpinmame}"
PINMAME_LIB_DIR="${PINMAME_LIB_DIR:-$VPINBALL_ROOT/third-party/runtime-libs/macos-arm64}"

mkdir -p "$ROOT/bin"

clang++ -std=c++17 \
  -I "$PINMAME_SRC" \
  "$ROOT/pinmame_exerciser.cpp" \
  -L "$PINMAME_LIB_DIR" \
  -lpinmame \
  -Wl,-rpath,"$PINMAME_LIB_DIR" \
  -o "$ROOT/bin/pinmame_exerciser"

echo "$ROOT/bin/pinmame_exerciser"

