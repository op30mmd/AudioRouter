#!/bin/bash
# AudioRouter - Build Client (Linux / Android Termux)
set -e

echo "================================================="
echo " Building AudioRouter Client"
echo "================================================="

# Get absolute path of this script's directory and project root
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ -f "$PROJECT_ROOT/Makefile" ] && [ -d "$PROJECT_ROOT/src" ]; then
    mkdir -p "$PROJECT_ROOT/bin" "$PROJECT_ROOT/build"
    # Termux ships clang++ by default; fall back to g++ when g++ is absent.
    if [ -z "${CXX:-}" ] && ! command -v g++ >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
        export CXX=clang++
    fi
    make -C "$PROJECT_ROOT" client
else
    echo "Error: source tree not found (no Makefile/src under $PROJECT_ROOT)." >&2
    echo "This is a build script for source checkouts - binary-only release" >&2
    echo "packages ship a prebuilt 'audiorouter_client' and do not need it." >&2
    exit 1
fi

echo ""
echo "================================================="
echo " Client built successfully: bin/audiorouter_client"
echo "================================================="
