#!/usr/bin/env bash
# Build and package the TypeScript desktop application without installing it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
npm --prefix "$ROOT/companion" ci
npm --prefix "$ROOT/companion" run build
npm --prefix "$ROOT/companion" run package -- "$@"
