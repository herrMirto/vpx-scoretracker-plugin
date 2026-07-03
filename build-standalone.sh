#!/usr/bin/env bash
# Builds scoretracker-plugin.dylib entirely outside of vpinball's own CMake
# project, using vpinball's third-party/include headers and its stock,
# unmodified libpinmame as an SDK reference only. Mirrors how a real user
# would install a prebuilt plugin release: compile once, drop the .dylib and
# plugin.cfg into the app bundle's Resources/plugins/scoretracker folder.
#
# Usage: ./build-standalone.sh [path-to-vpinball-checkout]
# Defaults to ~/vpinball.

set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VPINBALL_DIR="${1:-$HOME/vpinball}"

if [ ! -d "$VPINBALL_DIR/third-party/include" ]; then
  echo "error: $VPINBALL_DIR does not look like a vpinball checkout (missing third-party/include)" >&2
  exit 1
fi

APP_BUNDLE=$(find "$VPINBALL_DIR/build" -maxdepth 1 -iname "VPinballX*.app" | head -1)
if [ -z "$APP_BUNDLE" ]; then
  echo "error: no VPinballX*.app found under $VPINBALL_DIR/build" >&2
  exit 1
fi

ARCH=$(uname -m)
case "$ARCH" in
  arm64) ;;
  x86_64) ;;
  *) echo "error: unsupported host arch $ARCH" >&2; exit 1 ;;
esac

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

INCLUDES=(
  -I"$VPINBALL_DIR/third-party/include"
  -I"$VPINBALL_DIR/plugins"
  -I"$PLUGIN_DIR"
)
DEFINES=(-DIMGUI_DISABLE_DEBUG_TOOLS -DScoreTrackerPlugin_EXPORTS)
COMMON_FLAGS=(-O3 -DNDEBUG -arch "$ARCH" -mmacosx-version-min=14.0 -fPIC -Werror=return-local-addr)

echo "Compiling C sources..."
/usr/bin/cc -std=gnu99 "${COMMON_FLAGS[@]}" "${DEFINES[@]}" "${INCLUDES[@]}" \
  -c "$VPINBALL_DIR/third-party/include/mongoose/mongoose.c" -o "$WORK_DIR/mongoose.c.o"

echo "Compiling C++ sources..."
for src in PluginMain ScoreTracker B2STracker ScoresFileWriter; do
  /usr/bin/c++ -std=gnu++20 "${COMMON_FLAGS[@]}" "${DEFINES[@]}" "${INCLUDES[@]}" \
    -c "$PLUGIN_DIR/$src.cpp" -o "$WORK_DIR/$src.cpp.o"
done

echo "Linking scoretracker-plugin.dylib..."
/usr/bin/c++ -O3 -DNDEBUG -arch "$ARCH" -mmacosx-version-min=14.0 -bundle -Wl,-headerpad_max_install_names \
  -o "$WORK_DIR/scoretracker-plugin.dylib" \
  "$WORK_DIR/PluginMain.cpp.o" "$WORK_DIR/ScoreTracker.cpp.o" "$WORK_DIR/B2STracker.cpp.o" "$WORK_DIR/ScoresFileWriter.cpp.o" "$WORK_DIR/mongoose.c.o" \
  -L"$VPINBALL_DIR/third-party/runtime-libs/macos-$ARCH" \
  -Wl,-rpath,@executable_path/../Frameworks -lpinmame

DEST="$APP_BUNDLE/Contents/Resources/plugins/scoretracker"
mkdir -p "$DEST"
cp "$WORK_DIR/scoretracker-plugin.dylib" "$DEST/scoretracker-plugin.dylib"
cp "$PLUGIN_DIR/plugin.cfg" "$DEST/plugin.cfg"

echo "Installed to $DEST"
