#!/usr/bin/env bash
# Builds scoretracker-plugin.dylib entirely outside of vpinball's own CMake
# project, using vpinball's third-party/include headers and its stock,
# unmodified libpinmame as an SDK reference only. Mirrors how a real user
# would install a prebuilt plugin release: compile once, drop the .dylib and
# plugin.cfg into the app bundle's PlugIns/scoretracker folder.
#
# Usage: ./build-standalone.sh [path-to-vpinball-checkout] [path-to-nvram-maps] [pinmame-root]
# Defaults to ~/vpinball; when a maps path is given (a checkout of
# tomlogic/pinmame-nvram-maps or a compatible fork), its index.json, maps/ and
# platforms/ are installed to plugins/scoretracker/maps as the plugin's
# default maps. Pass the active PinMAME root as the third argument to also install the same
# snapshot to <pinmame-root>/memmaps, where current PinMAME versions decode and publish it.

set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VPINBALL_DIR="${1:-$HOME/vpinball}"
MAPS_DIR="${2:-}"
PINMAME_ROOT="${3:-}"

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
  -I"$PLUGIN_DIR/config"
  -iquote "$PLUGIN_DIR"
)
DEFINES=(-DScoreTrackerPlugin_EXPORTS)
COMMON_FLAGS=(-O3 -DNDEBUG -arch "$ARCH" -mmacosx-version-min=14.0 -fPIC)

echo "Compiling C++ sources..."
for src in ScoreTrackerPlugin NotificationOverlay B2STracker PinMameStateTracker ScoresFileWriter ScoreSignature common; do
  /usr/bin/c++ -std=gnu++20 "${COMMON_FLAGS[@]}" "${DEFINES[@]}" "${INCLUDES[@]}" \
    -c "$PLUGIN_DIR/$src.cpp" -o "$WORK_DIR/$src.cpp.o"
done

echo "Linking scoretracker-plugin.dylib..."
/usr/bin/c++ -O3 -DNDEBUG -arch "$ARCH" -mmacosx-version-min=14.0 -bundle -Wl,-headerpad_max_install_names \
  -o "$WORK_DIR/scoretracker-plugin.dylib" \
  "$WORK_DIR/ScoreTrackerPlugin.cpp.o" "$WORK_DIR/NotificationOverlay.cpp.o" "$WORK_DIR/B2STracker.cpp.o" "$WORK_DIR/PinMameStateTracker.cpp.o" "$WORK_DIR/ScoresFileWriter.cpp.o" "$WORK_DIR/ScoreSignature.cpp.o" "$WORK_DIR/common.cpp.o" \
  -L"$VPINBALL_DIR/third-party/runtime-libs/macos-$ARCH" \
  -Wl,-rpath,@executable_path/../Frameworks -lpinmame

DEST="$APP_BUNDLE/Contents/PlugIns/scoretracker"
mkdir -p "$DEST"
cp "$WORK_DIR/scoretracker-plugin.dylib" "$DEST/scoretracker-plugin.dylib"
cp "$PLUGIN_DIR/plugin.cfg" "$DEST/plugin.cfg"

if [ -n "$MAPS_DIR" ]; then
  if [ ! -f "$MAPS_DIR/index.json" ]; then
    echo "error: $MAPS_DIR does not look like an NVRAM maps checkout (missing index.json)" >&2
    exit 1
  fi
  mkdir -p "$DEST/maps"
  cp "$MAPS_DIR/index.json" "$DEST/maps/"
  [ -f "$MAPS_DIR/LICENSE" ] && cp "$MAPS_DIR/LICENSE" "$DEST/maps/"
  rsync -a --delete "$MAPS_DIR/maps/" "$DEST/maps/maps/"
  rsync -a --delete "$MAPS_DIR/platforms/" "$DEST/maps/platforms/"
  echo "Installed maps from $MAPS_DIR"

  if [ -n "$PINMAME_ROOT" ]; then
    PINMAME_MEMMAPS="$PINMAME_ROOT/memmaps"
    mkdir -p "$PINMAME_MEMMAPS/maps" "$PINMAME_MEMMAPS/platforms"
    MAPS_REAL="$(cd "$MAPS_DIR" && pwd -P)"
    PINMAME_MEMMAPS_REAL="$(cd "$PINMAME_MEMMAPS" && pwd -P)"
    if [ "$MAPS_REAL" = "$PINMAME_MEMMAPS_REAL" ]; then
      echo "PinMAME decoded-state maps already point to $MAPS_REAL"
    else
      cp "$MAPS_DIR/index.json" "$PINMAME_MEMMAPS/"
      [ -f "$MAPS_DIR/LICENSE" ] && cp "$MAPS_DIR/LICENSE" "$PINMAME_MEMMAPS/"
      rsync -a "$MAPS_DIR/maps/" "$PINMAME_MEMMAPS/maps/"
      rsync -a "$MAPS_DIR/platforms/" "$PINMAME_MEMMAPS/platforms/"
      echo "Installed PinMAME decoded-state maps to $PINMAME_MEMMAPS"
    fi
  else
    echo "PinMAME maps were not installed; pass its root as the third argument for live decoded-state testing"
  fi
fi

echo "Installed to $DEST"
