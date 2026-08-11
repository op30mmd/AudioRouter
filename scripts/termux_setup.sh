#!/data/data/com.termux/files/usr/bin/bash
# AudioRouter - Termux Android Client Setup Script
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

# Update package lists
echo "[1/4] Updating Termux package lists..."
if [ "$(id -u)" -ne 0 ]; then
    pkg update -y || true
else
    echo "     Skipped: pkg cannot run as root. Run 'pkg update' as the Termux user (without su)."
fi

# Install required build packages
echo "[2/4] Installing compiler and ALSA tools..."
if [ "$(id -u)" -ne 0 ]; then
    pkg install -y clang make alsa-utils || true
else
    echo "     Skipped: pkg cannot run as root. Run 'pkg install clang make alsa-utils' as the Termux user."
fi

# Build AudioRouter Client if not present
if [ -n "$BIN_PATH" ]; then
    echo "[3/4] Client binary already exists at: $BIN_PATH. Skipping compilation."
else
    if [ -f "$PROJECT_ROOT/Makefile" ]; then
        echo "[3/4] Compiling AudioRouter Client with clang++..."
        mkdir -p "$PROJECT_ROOT/bin" "$PROJECT_ROOT/build"
        CXX=clang++ make -C "$PROJECT_ROOT" client
    else
        echo "[3/4] Source code or Makefile not found, skipping compilation."
        echo "Please ensure the pre-compiled 'audiorouter_client' binary is present."
    fi
fi

echo "[4/4] Setup complete!"
echo ""
echo "To run AudioRouter Client with root privileges, execute:"
echo "  su"
echo "  ./bin/audiorouter_client -s <PC_IP_ADDRESS> -p 44100"
echo ""
echo "Or use the automated helper:"
echo "  ./scripts/termux_run.sh <PC_IP_ADDRESS>"
echo "================================================="
