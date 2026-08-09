#!/bin/bash
# AudioRouter - Build Client (Linux / Android Termux)
set -e

echo "================================================="
echo " Building AudioRouter Client"
echo "================================================="

# Get absolute path of this script's directory and project root
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ -f "$PROJECT_ROOT/Makefile" ]; then
    mkdir -p "$PROJECT_ROOT/bin" "$PROJECT_ROOT/build"
    make -C "$PROJECT_ROOT" client
else
    echo "Error: Makefile not found at $PROJECT_ROOT/Makefile"
    exit 1
fi

echo ""
echo "================================================="
echo " Client built successfully: bin/audiorouter_client"
echo "================================================="
