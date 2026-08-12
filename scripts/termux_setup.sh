#!/data/data/com.termux/files/usr/bin/bash
# AudioRouter - Termux Android Client Setup Script
#
# Works in BOTH deployment layouts:
#   - source checkout:      ./scripts/termux_setup.sh
#       Installs the build toolchain (clang, make, optional ndk-sysroot) and
#       compiles the client when no binary is present.
#   - binary-only release:  ./termux_setup.sh
#       Installs runtime tools only (alsa-utils for tinymix speaker routing)
#       and uses the prebuilt 'audiorouter_client' as-is. The script never
#       tries to compile when the source tree is absent.
set -e

echo "================================================="
echo " Setting up AudioRouter in Termux (Android ALSA)"
echo "================================================="

# Get absolute path of this script's directory and project root
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Define candidate paths for the client binary
CANDIDATE_1="$PROJECT_ROOT/bin/audiorouter_client"
CANDIDATE_2="$SCRIPT_DIR/audiorouter_client"
CANDIDATE_3="$PROJECT_ROOT/audiorouter_client"

if [ -f "$CANDIDATE_1" ]; then
    BIN_PATH="$CANDIDATE_1"
elif [ -f "$CANDIDATE_2" ]; then
    BIN_PATH="$CANDIDATE_2"
elif [ -f "$CANDIDATE_3" ]; then
    BIN_PATH="$CANDIDATE_3"
elif command -v audiorouter_client >/dev/null 2>&1; then
    BIN_PATH="$(command -v audiorouter_client)"
else
    BIN_PATH=""
fi

# Full source tree present? (buildable checkout vs binary-only release)
HAS_SOURCE=0
if [ -f "$PROJECT_ROOT/Makefile" ] && [ -d "$PROJECT_ROOT/src" ]; then
    HAS_SOURCE=1
fi

# Update package lists
echo "[1/4] Updating Termux package lists..."
if [ "$(id -u)" -ne 0 ]; then
    pkg update -y || true
else
    echo "     Skipped: pkg cannot run as root. Run 'pkg update' as the Termux user (without su)."
fi

# Install packages
if [ "$HAS_SOURCE" -eq 1 ]; then
    echo "[2/4] Source checkout: installing compiler and ALSA tools..."
    if [ "$(id -u)" -ne 0 ]; then
        pkg install -y clang make alsa-utils || true
        # Optional: NDK sysroot with Android platform stub libraries. Needed only
        # to link the AAudio no-root backend (-laaudio); skipped when unavailable.
        pkg install -y ndk-sysroot 2>/dev/null || true
    else
        echo "     Skipped: pkg cannot run as root. Run 'pkg install clang make alsa-utils' as the Termux user."
    fi
else
    echo "[2/4] Binary-only package: installing runtime tools only (tinymix for mixer routing)..."
    if [ "$(id -u)" -ne 0 ]; then
        pkg install -y alsa-utils || true
    else
        echo "     Skipped: pkg cannot run as root. Run 'pkg install alsa-utils' as the Termux user."
    fi
fi

# Ensure the client binary is available
if [ -n "$BIN_PATH" ]; then
    echo "[3/4] Client binary present: $BIN_PATH"
elif [ "$HAS_SOURCE" -eq 1 ]; then
    echo "[3/4] Compiling AudioRouter Client with clang++..."
    mkdir -p "$PROJECT_ROOT/bin" "$PROJECT_ROOT/build"
    CXX=clang++ make -C "$PROJECT_ROOT" client
    BIN_PATH="$PROJECT_ROOT/bin/audiorouter_client"
else
    echo "[3/4] ERROR: no 'audiorouter_client' binary found." >&2
    echo "    This is a binary-only package (no source tree), so it cannot compile." >&2
    echo "    Download the latest Android release from GitHub Releases and place" >&2
    echo "    'audiorouter_client' next to this script (or in bin/, or in PATH)." >&2
    exit 1
fi

echo "[4/4] Setup complete!"
echo ""
if [ -x "$SCRIPT_DIR/audiorouter_client" ]; then
    # Release layout: the binary sits next to this script.
    echo "To run AudioRouter Client:"
    echo "  ./audiorouter_client -s <PC_IP_ADDRESS> -p 44100"
    echo "  (or: ./termux_run.sh <PC_IP_ADDRESS>)"
    if [ -x "$SCRIPT_DIR/stream_daemon" ]; then
        echo ""
        echo "Standalone AAudio FIFO daemon also present:"
        echo "  ./stream_daemon"
    fi
else
    echo "To run AudioRouter Client with root privileges, execute:"
    echo "  su"
    echo "  ./bin/audiorouter_client -s <PC_IP_ADDRESS> -p 44100"
    echo ""
    echo "Or use the automated helper:"
    echo "  ./scripts/termux_run.sh <PC_IP_ADDRESS>"
fi
echo "================================================="
