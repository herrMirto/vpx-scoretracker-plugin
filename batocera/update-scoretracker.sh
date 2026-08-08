#!/bin/bash

set -euo pipefail

INSTALL_ROOT="${SCORETRACKER_INSTALL_ROOT:-/userdata/system/scoretracker}"
REPOSITORY="herrMirto/vpx-scoretracker-plugin"
RELEASE_API="${SCORETRACKER_RELEASE_API_URL:-https://api.github.com/repos/$REPOSITORY/releases/latest}"
DOWNLOAD_PREFIX="https://github.com/$REPOSITORY/releases/download/"
BUILD_INFO="$INSTALL_ROOT/build-info.json"
LOCK_FILE="$INSTALL_ROOT/update.lock"
LOG_FILE="$INSTALL_ROOT/scoretracker-update.log"

fail() {
    echo "ScoreTracker update failed: $*" >&2
    exit 1
}

action="${1:---check}"
case "$action" in
    --check|--install) ;;
    --help|-h)
        echo "Usage: $0 [--check|--install]"
        exit 0
        ;;
    *) fail "unknown option: $action" ;;
esac

[ "$(uname -m)" = "x86_64" ] || fail "this updater supports Batocera x86-64 only"
command -v curl >/dev/null 2>&1 || fail "curl is not available"
command -v jq >/dev/null 2>&1 || fail "jq is not available"
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum is not available"
command -v flock >/dev/null 2>&1 || fail "flock is not available"

mkdir -p "$INSTALL_ROOT"
exec 9>"$LOCK_FILE"
flock -n 9 || fail "another ScoreTracker update is already running"

release_json="$(curl --fail --silent --show-error \
    -H 'Accept: application/vnd.github+json' \
    -H 'User-Agent: VPX-ScoreTracker-Batocera-Updater' \
    "$RELEASE_API")" || fail "could not read the latest GitHub release"

latest_version="$(printf '%s' "$release_json" | jq -r '.tag_name // ""')"
latest_version="${latest_version#v}"
latest_version="${latest_version#V}"
printf '%s\n' "$latest_version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' \
    || fail "the latest release has an invalid version"

current_version="0.0.0"
if [ -f "$BUILD_INFO" ]; then
    current_version="$(jq -r '.version // "0.0.0"' "$BUILD_INFO")"
fi

asset_name="scoretracker-batocera-${latest_version}-x86_64.tar.gz"
asset_url="$(printf '%s' "$release_json" | jq -r --arg name "$asset_name" \
    '.assets[]? | select(.name == $name) | .browser_download_url' | head -n 1)"
asset_digest="$(printf '%s' "$release_json" | jq -r --arg name "$asset_name" \
    '.assets[]? | select(.name == $name) | .digest // ""' | head -n 1)"

[ -n "$asset_url" ] || fail "release $latest_version has no Batocera x86-64 package"
if [ "$RELEASE_API" = "https://api.github.com/repos/$REPOSITORY/releases/latest" ]; then
    case "$asset_url" in
        "$DOWNLOAD_PREFIX"*) ;;
        *) fail "the release returned an unexpected download URL" ;;
    esac
fi
printf '%s\n' "$asset_digest" | grep -Eq '^sha256:[0-9a-fA-F]{64}$' \
    || fail "the release does not provide a valid SHA-256 digest"

if ! python3 -c 'import sys; p=lambda value: tuple(map(int, value.split("."))); raise SystemExit(0 if p(sys.argv[1]) > p(sys.argv[2]) else 1)' "$latest_version" "$current_version"; then
    echo "UP_TO_DATE=$current_version"
    exit 0
fi

echo "UPDATE_AVAILABLE=$latest_version"
[ "$action" = "--install" ] || exit 0

work_dir="$(mktemp -d "$INSTALL_ROOT/.update.XXXXXX")"
trap 'rm -rf "$work_dir"' EXIT
archive="$work_dir/$asset_name"

echo "Downloading ScoreTracker $latest_version..." | tee -a "$LOG_FILE"
curl --fail --location --silent --show-error "$asset_url" -o "$archive"
expected_hash="${asset_digest#sha256:}"
actual_hash="$(sha256sum "$archive" | awk '{print $1}')"
[ "${actual_hash,,}" = "${expected_hash,,}" ] || fail "the downloaded package failed SHA-256 verification"

tar -xzf "$archive" -C "$work_dir"
package="$work_dir/scoretracker-install"
[ -x "$package/install-batocera.sh" ] || fail "the downloaded package is missing its installer"
[ -f "$package/build-info.json" ] || fail "the downloaded package has no build information"
package_version="$(jq -r '.version // ""' "$package/build-info.json")"
[ "$package_version" = "$latest_version" ] || fail "package version $package_version does not match release $latest_version"

echo "Installing ScoreTracker $latest_version..." | tee -a "$LOG_FILE"
"$package/install-batocera.sh" 2>&1 | tee -a "$LOG_FILE"
echo "ScoreTracker was updated to $latest_version." | tee -a "$LOG_FILE"
