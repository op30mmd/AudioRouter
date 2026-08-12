#!/data/data/com.termux/files/usr/bin/bash
# AudioRouter - Termux Android Client Runner Script
# Usage: ./scripts/termux_run.sh [-s SERVER_IP] [-p PORT] [CLIENT_ARGS...]
# Backward-compatible: ./scripts/termux_run.sh [SERVER_IP] [PORT] [CLIENT_ARGS...]
# e.g. ./scripts/termux_run.sh 10.16.211.80 44100 -d agm -b auto
#      ./scripts/termux_run.sh -s 10.58.30.80 -d aaudio -b auto
#
# The client is ALWAYS launched through its ABSOLUTE path, wrapped in
#     su -c "/data/data/com.termux/files/home/AudioRouter/bin/audiorouter_client <args>"
# This is required for the AAudio build: it links /system/lib64/libaaudio.so
# by absolute path, which only the Android/system dynamic linker resolves.
# Launching the binary from the Termux shell environment fails at runtime
# with linker errors, so no LD_LIBRARY_PATH/HOME/chmod preamble is injected
# into the su command - the plain absolute-path invocation is what works.

SERVER_IP=""
PORT="44100"
declare -a CLIENT_ARGS=()

POS=0
while [ $# -gt 0 ]; do
    case "$1" in
        -s|--server)
            if [ $# -lt 2 ]; then
                echo "Error: $1 requires an argument (server IP)." >&2
                exit 1
            fi
            SERVER_IP="$2"
            POS=2
            shift 2
            ;;
        -p|--port)
            if [ $# -lt 2 ]; then
                echo "Error: $1 requires an argument (port)." >&2
                exit 1
            fi
            PORT="$2"
            POS=2
            shift 2
            ;;
        -d|--device|-b|--bind|-l|--latency)
            if [ $# -lt 2 ]; then
                echo "Error: $1 requires an argument." >&2
                exit 1
            fi
            CLIENT_ARGS+=("$1" "$2")
            shift 2
            ;;
        -*)
            CLIENT_ARGS+=("$1")
            shift
            ;;
        *)
            if [ "$POS" -eq 0 ]; then
                SERVER_IP="$1"
                POS=1
            elif [ "$POS" -eq 1 ]; then
                PORT="$1"
                POS=2
            else
                CLIENT_ARGS+=("$1")
            fi
            shift
            ;;
    esac
done

case "$PORT" in
    (*[!0-9]*)
        echo "Error: Port '$PORT' is not a valid number." >&2
        exit 1
        ;;
esac

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
    HAS_DISCOVER=0
    for a in "${CLIENT_ARGS[@]}"; do
        if [ "$a" = "--discover" ]; then
            HAS_DISCOVER=1
        fi
    done
    if [ "$HAS_DISCOVER" -eq 0 ]; then
        echo "================================================="
        echo " AudioRouter Android ALSA Client"
        echo "================================================="
        echo ""
        echo "Enter Windows PC IP address (e.g. 192.168.43.1 or 192.168.137.1):"
        read -r SERVER_IP
    fi
fi

if [ -z "$SERVER_IP" ]; then
    echo "Error: No Server IP specified. Exiting."
    exit 1
fi

# Resolve the binary to a plain absolute path (su -c needs one).
ABS_BIN="$BIN_PATH"
if [ "${ABS_BIN#/}" = "$ABS_BIN" ]; then
    ABS_BIN="$(cd "$(dirname "$ABS_BIN")" && pwd)/$(basename "$ABS_BIN")"
fi

# Quote a single argument for the mksh command string passed to su -c.
sh_quote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

# Build the exact command: su -c "/abs/path/audiorouter_client <args>"
CMD="$ABS_BIN -s $(sh_quote "$SERVER_IP") -p $(sh_quote "$PORT")"
for a in "${CLIENT_ARGS[@]}"; do
    CMD="$CMD $(sh_quote "$a")"
done

echo "Connecting to Windows Server at $SERVER_IP:$PORT..."
echo "Running: $CMD"

if [ "$(id -u)" -ne 0 ]; then
    if ! command -v su >/dev/null 2>&1; then
        echo "Error: not running as root and 'su' is not available. Run 'su' first or install su." >&2
        exit 1
    fi
    echo "Requesting root privileges via su..."
    su -c "$CMD"
else
    # Already root (Android/system shell): launch the absolute path directly.
    exec "$ABS_BIN" -s "$SERVER_IP" -p "$PORT" "${CLIENT_ARGS[@]}"
fi
