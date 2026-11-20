#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec npm --prefix "$ROOT/companion" start -- "$@"
