#!/data/data/com.termux/files/usr/bin/bash
# AudioRouter - Termux Android Client Runner Script
# Usage: ./scripts/termux_run.sh [SERVER_IP] [PORT]

SERVER_IP="${1:-}"
PORT="${2:-44100}"
BIN_PATH="$(dirname "$0")/../bin/audiorouter_client"

if [ ! -f "$BIN_PATH" ]; then
    echo "Client binary not found at $BIN_PATH. Compiling now..."
    make client
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
    su -c "chmod 666 /dev/snd/* 2>/dev/null; $BIN_PATH -s $SERVER_IP -p $PORT"
else
    chmod 666 /dev/snd/* 2>/dev/null || true
    "$BIN_PATH" -s "$SERVER_IP" -p "$PORT"
fi
