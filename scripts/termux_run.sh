#!/data/data/com.termux/files/usr/bin/bash
# AudioRouter - Termux Android Client Runner Script
# Works in both a source checkout (compiles the client if missing) and a
# binary-only release artifact (uses the prebuilt 'audiorouter_client' next
# to this script - never tries to compile without a source tree).
# Usage: ./scripts/termux_run.sh [-s SERVER_IP] [-p PORT] [CLIENT_ARGS...]
# Backward-compatible: ./scripts/termux_run.sh [SERVER_IP] [PORT] [CLIENT_ARGS...]
# e.g. ./scripts/termux_run.sh 10.16.211.80 44100 -d agm -b auto
#      ./scripts/termux_run.sh -s 10.58.30.80 -d aaudio -b auto
#
# The client is ALWAYS launched through its ABSOLUTE path.
#   - Root-requiring backends (ALSA/AGM/direct): wrapped in
#       su -c "/data/data/com.termux/files/home/AudioRouter/bin/audiorouter_client <args>"
#     The AAudio build links /system/lib64/libaaudio.so by absolute path, which
#     only the Android/system dynamic linker resolves, so the su command is the
#     plain absolute-path invocation without any LD_LIBRARY_PATH/HOME/chmod
#     preamble.
#   - AAudio (-d aaudio / aaudio:*): runs IN-PROCESS, exactly like the
#     standalone stream_daemon - which is proven to work as root
#     (sudo ./stream_daemon + ffmpeg > pipe plays audio). So the client is
#     launched the same way regardless: with -b/--bind via su (root for
#     SO_BINDTODEVICE), without -b directly as the current user.

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
    # Build from source only when a source checkout is present; a binary-only
    # release cannot compile, so point the user at the prebuilt artifact.
    if [ -f "$PROJECT_ROOT/Makefile" ] && [ -d "$PROJECT_ROOT/src" ]; then
        echo "Client binary not found. Compiling now..."
        mkdir -p "$PROJECT_ROOT/bin" "$PROJECT_ROOT/build"
        make -C "$PROJECT_ROOT" client
        BIN_PATH="$PROJECT_ROOT/bin/audiorouter_client"
    else
        echo "Error: 'audiorouter_client' not found." >&2
        echo "This is a binary-only package (no source tree to compile from)." >&2
        echo "Download the Android release binary from GitHub Releases and place it" >&2
        echo "next to this script, in 'bin/', or in PATH." >&2
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

# AAudio runs in-process like the stream_daemon (which works as root), so:
#   - aaudio + -b/--bind  -> run via su (root) for SO_BINDTODEVICE.
#   - aaudio without -b   -> run directly as the current user.
IS_AAUDIO=0
HAS_BIND=0
for a in "${CLIENT_ARGS[@]}"; do
    case "$a" in
        aaudio|aaudio:*) IS_AAUDIO=1 ;;
        -b|--bind) HAS_BIND=1 ;;
    esac
done

# Run the client under 'su -c' with a SIGNAL BRIDGE. Magisk's su can place
# the command in its own session, so the terminal's Ctrl+C (SIGINT to the
# foreground process group) never reaches the client - which is why Ctrl+C
# appears ignored even though the client handles SIGINT. Solution: run su in
# the background (same process group), trap INT/TERM in this script, and
# forward the signal to the client, escalating INT -> TERM -> KILL.
#
# CRITICAL: the client runs as ROOT (via su) while this script runs as the
# non-root Termux app user. Android (uid rules + SELinux) silently refuses
# (EPERM) a non-root process signaling a root process, so a plain pkill here
# does nothing. The signal must be delivered AS ROOT, through su itself:
#   su -c "pkill -<sig> -f '<client path>'"
run_via_su() {
    local cmd="$1"
    su -c "$cmd" &
    local su_pid=$!

    # Pattern that matches the client's command line but NOT this script's own
    # pkill/su command lines: "[a]udiorouter_client" as a regex matches the
    # real path, while the literal "[a]..." in our own cmdline does not match.
    local base pat
    base="$(basename "$ABS_BIN")"
    pat="[${base%${base#?}}]${base#?}"

    signal_client() {
        local sig="$1"
        # Direct (works when this script is root, or the client is same-uid).
        pkill -"$sig" -f "$pat" 2>/dev/null
        # The real delivery path for the su case: signal AS ROOT.
        su -c "pkill -$sig -f '$pat'" 2>/dev/null
    }

    cleanup() {
        trap - INT TERM   # no re-entry while we are tearing down
        echo ""
        echo "Stopping AudioRouter client..."
        kill -INT "$su_pid" 2>/dev/null
        signal_client INT
        sleep 2
        signal_client TERM
        sleep 1
        signal_client KILL
    }
    trap cleanup INT TERM
    wait "$su_pid"
    local status=$?
    trap - INT TERM
    return $status
}

if [ "$(id -u)" -ne 0 ]; then
    if [ "$IS_AAUDIO" -eq 1 ] && [ "$HAS_BIND" -eq 1 ]; then
        # Root for the socket binding; AAudio runs in-process like the
        # stream_daemon (which works as root).
        if ! command -v su >/dev/null 2>&1; then
            echo "Error: '-b' needs root but 'su' is not available. Run 'su' first or install su." >&2
            exit 1
        fi
        echo "Requesting root privileges via su (for -b auto); AAudio runs in-process like stream_daemon..."
        run_via_su "$CMD"
    elif [ "$IS_AAUDIO" -eq 1 ]; then
        echo "AAudio backend: running as the current user (like: ./stream_daemon)."
        echo "Note: if AAudio does not work on this device, the AGM/ALSA fallbacks need root -"
        echo "re-run with '-b auto' (or '-d agm') via su in that case."
        exec "$ABS_BIN" -s "$SERVER_IP" -p "$PORT" "${CLIENT_ARGS[@]}"
    else
        if ! command -v su >/dev/null 2>&1; then
            echo "Error: not running as root and 'su' is not available. Run 'su' first or install su." >&2
            exit 1
        fi
        echo "Requesting root privileges via su..."
        run_via_su "$CMD"
    fi
elif [ "$IS_AAUDIO" -eq 1 ]; then
    # Already root (e.g. a root shell) and AAudio requested: launch directly;
    # AAudio runs in-process like the stream_daemon (which works as root).
    exec "$ABS_BIN" -s "$SERVER_IP" -p "$PORT" "${CLIENT_ARGS[@]}"
else
    # Already root (Android/system shell) with a root-requiring backend:
    # launch the absolute path directly.
    exec "$ABS_BIN" -s "$SERVER_IP" -p "$PORT" "${CLIENT_ARGS[@]}"
fi
