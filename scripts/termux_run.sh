#!/data/data/com.termux/files/usr/bin/bash
# AudioRouter - Termux Android Client Runner Script
# Usage: ./scripts/termux_run.sh [SERVER_IP] [PORT]

SERVER_IP="${1:-}"
PORT="${2:-44100}"

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

if [ -z "$BIN_PATH" ]; then
    if [ -f "$PROJECT_ROOT/Makefile" ]; then
        echo "Client binary not found. Compiling now..."
        mkdir -p "$PROJECT_ROOT/bin" "$PROJECT_ROOT/build"
        make -C "$PROJECT_ROOT" client
        BIN_PATH="$PROJECT_ROOT/bin/audiorouter_client"
    else
        echo "Error: Client binary not found and source code / Makefile is missing."
        echo "Please compile the client first or place the 'audiorouter_client' binary in 'bin/' or PATH."
        exit 1
    fi
fi

if [ -z "$SERVER_IP" ]; then
    echo "================================================="
    echo " AudioRouter Android ALSA Client"
    echo "================================================="
    echo ""
    echo "Enter Windows PC IP address (e.g. 192.168.43.1 or 192.168.137.1):"
    read -r SERVER_IP
fi

if [ -z "$SERVER_IP" ]; then
    echo "Error: No Server IP specified. Exiting."
    exit 1
fi

echo "Connecting to Windows Server at $SERVER_IP:$PORT..."

# Run with root if possible
if [ "$(id -u)" -ne 0 ]; then
    echo "Requesting root privileges via su..."
    su -c "chmod 666 /dev/snd/* 2>/dev/null; export HOME=\${HOME:-/data/data/com.termux/files/home}; export LD_LIBRARY_PATH=/vendor/lib64:\$LD_LIBRARY_PATH; $BIN_PATH -s $SERVER_IP -p $PORT ${@:3}"
else
    chmod 666 /dev/snd/* 2>/dev/null || true
    export LD_LIBRARY_PATH=/vendor/lib64:$LD_LIBRARY_PATH
    "$BIN_PATH" -s "$SERVER_IP" -p "$PORT" "${@:3}"
fi
