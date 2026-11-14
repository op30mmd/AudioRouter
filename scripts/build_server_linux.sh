#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "Building AudioRouter Linux server..."
make -j"$(nproc 2>/dev/null || echo 4)" server

echo "Done: $ROOT_DIR/bin/audiorouter_server"
