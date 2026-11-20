#!/usr/bin/env bash
# ScoreTracker uses only public headers; native notifications are owned by VPX.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/platforms/config.sh"
mkdir -p "$ROOT/external" "$ROOT/third-party/include"
if [[ -n "${VPINBALL_SOURCE_DIR:-}" ]]; then
  VPX_SRC="$VPINBALL_SOURCE_DIR"
  SDK_REVISION=$(git -C "$VPX_SRC" rev-parse HEAD)
  echo "Using VPX SDK revision: $SDK_REVISION"
else
  VPX_SRC="$ROOT/external/vpinball-$VPINBALL_SHA"
  SDK_REVISION="$VPINBALL_SHA"
  if [[ ! -f "$VPX_SRC/plugins/plugins/VPXPlugin.h" ]]; then
    curl --fail --location --retry 3 "https://codeload.github.com/vpinball/vpinball/tar.gz/$VPINBALL_SHA" -o "$ROOT/external/vpinball.tar.gz"
    tar -xzf "$ROOT/external/vpinball.tar.gz" -C "$ROOT/external" \
      "vpinball-$VPINBALL_SHA/plugins/plugins" "vpinball-$VPINBALL_SHA/third-party/include/nlohmann" "vpinball-$VPINBALL_SHA/LICENSE"
    rm "$ROOT/external/vpinball.tar.gz"
  fi
fi
mkdir -p "$ROOT/third-party/include/plugins"
cp "$VPX_SRC"/plugins/plugins/*.h "$ROOT/third-party/include/plugins/"
rm -rf "$ROOT/third-party/include/nlohmann"
cp -R "$VPX_SRC/third-party/include/nlohmann" "$ROOT/third-party/include/"
printf 'VPINBALL_SHA=%s\n' "$SDK_REVISION" > "$ROOT/third-party/include/revisions.txt"
echo "Staged ScoreTracker SDK headers in third-party/include"
