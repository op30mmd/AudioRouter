#!/bin/bash
# AudioRouter - Build the standalone AAudio FIFO stream daemon for Android
#
# Produces bin/stream_daemon (Android ARM64, API 30). Requires either:
#   - an Android NDK (ANDROID_NDK_HOME / ANDROID_NDK_LATEST_HOME / ANDROID_SDK_ROOT), or
#   - Termux's clang++ (targets Android by default)
#
# No root required at runtime: AAudio plays through Android's audio HAL
# (works on stock devices, Android 8.0+).
set -e

echo "================================================="
echo " Building AudioRouter AAudio Stream Daemon"
echo "================================================="

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
mkdir -p "$PROJECT_ROOT/bin"

if [ ! -f "$PROJECT_ROOT/src/tools/stream_daemon.cpp" ]; then
    echo "Error: source file src/tools/stream_daemon.cpp not found." >&2
    echo "This is a build script for source checkouts - binary-only release" >&2
    echo "packages ship a prebuilt 'stream_daemon' and do not need it." >&2
    exit 1
fi

CXX=""
if [ -n "$ANDROID_NDK_LATEST_HOME" ] && [ -f "$ANDROID_NDK_LATEST_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++" ]; then
    CXX="$ANDROID_NDK_LATEST_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++"
elif [ -n "$ANDROID_NDK_HOME" ] && [ -f "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++" ]; then
    CXX="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++"
elif command -v clang++ >/dev/null 2>&1; then
    CXX=clang++   # Termux clang++ targets Android by default
else
    echo "Error: no Android-capable clang++ found."
    echo "Install the Android NDK (set ANDROID_NDK_HOME) or run this inside Termux."
    exit 1
fi

# NDK clang++ uses its own sysroot automatically (relative to its install dir).
# -target aarch64-linux-android30 matches the API level of stock Termux devices.
"$CXX" -O2 -Wno-unavailable-declarations -target aarch64-linux-android30 \
    -o "$PROJECT_ROOT/bin/stream_daemon" "$PROJECT_ROOT/src/tools/stream_daemon.cpp" \
    -laaudio -lm

echo ""
echo "================================================="
echo " Built successfully: bin/stream_daemon"
echo "================================================="
echo ""
echo "Usage:"
echo "  ./bin/stream_daemon                        # pipe: /data/local/tmp/audio_pipe, 48 kHz"
echo "  ./bin/stream_daemon /data/local/tmp/p 44100"
echo ""
echo "Feed it raw interleaved S16 stereo PCM:"
echo "  cat audio.raw > /data/local/tmp/audio_pipe"
