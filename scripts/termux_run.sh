#!/data/data/com.termux/files/usr/bin/bash
# AudioRouter - Termux Android Client Runner Script
# Works in both a source checkout (compiles the client if missing) and a
# binary-only release artifact (uses the prebuilt 'audiorouter_client' next
# to this script - never tries to compile without a source tree).
# Usage: ./scripts/termux_run.sh [-s SERVER_IP] [-p PORT] [CLIENT_ARGS...]
# Backward-compatible: ./scripts/termux_run.sh [SERVER_IP] [PORT] [CLIENT_ARGS...]
# e.g. ./scripts/termux_run.sh 10.16.211.80 44100 -d agm -b auto
#      ./scripts/termux_run.sh -s 10.58.30.80 -d aaudio -b auto
#      ./scripts/termux_run.sh -s 10.58.30.80 -d termux   (Termux:API media
#                                                player, no root - needs the
#                                                Termux:API app installed)
#      ./scripts/termux_run.sh -u -d aaudio     (Voice over USB: no server IP)
#      ./scripts/termux_run.sh --tether -d agm  (USB tethering: native UDP over
#                                                the cable, lowest latency - the
#                                                phone switches to RNDIS and back
#                                                automatically; needs root)
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
USB_MODE=0
TETHER=0
LATENCY_EXPLICIT=0
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
            if [ "$1" = "-l" ] || [ "$1" = "--latency" ]; then
                LATENCY_EXPLICIT=1
            fi
            CLIENT_ARGS+=("$1" "$2")
            shift 2
            ;;
        -u|--usb)
            USB_MODE=1
            CLIENT_ARGS+=("$1")
            shift
            ;;
        --tether)
            TETHER=1
            shift
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

# USB mode streams over the adb reverse tunnel - no server IP involved.
# --discover lets the client find the server on the LAN - no IP either.
# --tether implies discovery over the RNDIS cable.
if [ "$TETHER" -eq 1 ]; then
    if [ "$USB_MODE" -eq 1 ]; then
        echo "Error: --tether and --usb are mutually exclusive (pick one USB transport)." >&2
        exit 1
    fi
    HAS_DISCOVER=1
else
    HAS_DISCOVER=0
fi
for a in "${CLIENT_ARGS[@]}"; do
    if [ "$a" = "--discover" ]; then
        HAS_DISCOVER=1
    fi
done

if [ -z "$SERVER_IP" ] && [ "$USB_MODE" -eq 0 ] && [ "$HAS_DISCOVER" -eq 0 ]; then
    echo "================================================="
    echo " AudioRouter Android ALSA Client"
    echo "================================================="
    echo ""
    echo "Enter Windows PC IP address (e.g. 192.168.43.1 or 192.168.137.1):"
    read -r SERVER_IP
fi

if [ -z "$SERVER_IP" ] && [ "$USB_MODE" -eq 0 ] && [ "$HAS_DISCOVER" -eq 0 ]; then
    echo "Error: No Server IP specified. Exiting."
    exit 1
fi

if [ "$USB_MODE" -eq 1 ] && [ -n "$SERVER_IP" ]; then
    echo "Warning: -s/--server ($SERVER_IP) is ignored in USB mode; the stream goes over the USB cable via adb reverse." >&2
fi
if [ "$TETHER" -eq 1 ] && [ -n "$SERVER_IP" ]; then
    echo "Warning: -s/--server ($SERVER_IP) is ignored in tethering mode; the server is discovered over the USB cable." >&2
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
# USB mode passes no -s: the client connects through the adb reverse tunnel.
# --discover passes no -s either: the client finds the server on its own.
if [ "$USB_MODE" -eq 1 ] || [ -z "$SERVER_IP" ]; then
    CMD="$ABS_BIN -p $(sh_quote "$PORT")"
    FINAL_ARGS=(-p "$PORT")
else
    CMD="$ABS_BIN -s $(sh_quote "$SERVER_IP") -p $(sh_quote "$PORT")"
    FINAL_ARGS=(-s "$SERVER_IP" -p "$PORT")
fi
for a in "${CLIENT_ARGS[@]}"; do
    CMD="$CMD $(sh_quote "$a")"
    FINAL_ARGS+=("$a")
done

# Tethering mode: pin the socket to the RNDIS interface (root) and discover
# the server over the cable. Unicast routes on-link via rndis0 by itself, but
# the DISCOVERY broadcast would follow the default route (a VPN would swallow
# it), so the interface pin is what makes discovery reliable.
# The RNDIS link has a slow ~130 ms transit oscillation (measured on-device),
# so the LAN default (35 ms) starves; default to 100 ms unless -l was given.
if [ "$TETHER" -eq 1 ]; then
    FINAL_ARGS=(-b rndis0 --discover "${FINAL_ARGS[@]}")
    if [ "$LATENCY_EXPLICIT" -eq 0 ]; then
        FINAL_ARGS+=(-l 100)
    fi
    CMD="$ABS_BIN"
    for a in "${FINAL_ARGS[@]}"; do
        CMD="$CMD $(sh_quote "$a")"
    done
fi

if [ "$TETHER" -eq 1 ]; then
    echo "USB tethering mode: native UDP over the cable via RNDIS (no adb, lowest latency)."
    echo "On the PC (one-time): run scripts\\usb_tether_setup.bat while tethering is active -"
    echo "  it keeps the USB link off your internet routing (no default gateway via the phone)."
    echo "Then just start: bin\\audiorouter_server.exe   (it binds 0.0.0.0 and answers discovery)"
    echo "Jitter buffer: 100 ms by default (the RNDIS link has a slow transit oscillation; -l overrides)."
elif [ "$USB_MODE" -eq 1 ]; then
    echo "USB mode: streaming over the USB cable via adb reverse tcp:$PORT tcp:$PORT"
    echo "On the PC run first: scripts\\usb_setup.bat   (or: adb reverse tcp:$PORT tcp:$PORT)"
elif [ "$HAS_DISCOVER" -eq 1 ]; then
    echo "Auto-discovering the Windows Server on the local network (port $PORT)..."
else
    echo "Connecting to Windows Server at $SERVER_IP:$PORT..."
fi
echo "Running: $CMD"

# Tethering setup: switch the USB connection to RNDIS (the phone becomes a
# USB network device and the PC gets a link-local lease from it), wait for
# rndis0 to come up, and restore the adb function when the client exits.
if [ "$TETHER" -eq 1 ]; then
    if ! command -v su >/dev/null 2>&1; then
        echo "Error: --tether needs root (su) to switch the USB function and pin the socket." >&2
        exit 1
    fi
    echo "Switching the USB connection to RNDIS (tethering)..."
    if ! su -c "svc usb setFunctions rndis" >/dev/null 2>&1; then
        echo "Error: could not switch the USB function to RNDIS." >&2
        echo "Enable 'USB tethering' in the phone Settings manually, then re-run." >&2
        exit 1
    fi
    RNDIS_UP=0
    for i in 1 2 3 4 5 6 7 8; do
        if su -c "/system/bin/ip link show rndis0" >/dev/null 2>&1; then
            RNDIS_UP=1
            break
        fi
        sleep 1
    done
    if [ "$RNDIS_UP" -eq 0 ]; then
        echo "Error: rndis0 did not come up." >&2
        echo "Restore the adb connection with: su -c 'svc usb setFunctions adb'" >&2
        su -c "svc usb setFunctions adb" >/dev/null 2>&1
        exit 1
    fi
    echo "RNDIS link is up; discovering the server over the cable..."
    restore_usb() {
        trap - EXIT
        echo ""
        echo "Restoring the USB connection (adb)..."
        su -c "svc usb setFunctions adb" >/dev/null 2>&1
    }
    trap restore_usb EXIT
fi

# AAudio runs in-process like the stream_daemon (which works as root), and the
# Termux:API backend must run as the Termux app user (its listen-socket
# protocol only accepts the Termux uid, and com.termux.api reads the segment
# files from the Termux home), so:
#   - aaudio/termux + -b/--bind  -> run via su (root) for SO_BINDTODEVICE.
#   - aaudio/termux without -b   -> run directly as the current user.
IS_AAUDIO=0
IS_TERMUX=0
HAS_BIND=0
for a in "${CLIENT_ARGS[@]}"; do
    case "$a" in
        aaudio|aaudio:*) IS_AAUDIO=1 ;;
        termux|termux-api|termux:*) IS_TERMUX=1 ;;
        -b|--bind) HAS_BIND=1 ;;
    esac
done
# Tethering always pins the socket (the -b rndis0 prepended above).
if [ "$TETHER" -eq 1 ]; then
    HAS_BIND=1
fi

# AAudio renders only as a non-root app user, but the rndis0 pin needs root -
# the two cannot be combined. Point the user at the manual -s flow instead.
if [ "$TETHER" -eq 1 ] && [ "$IS_AAUDIO" -eq 1 ] && [ "$(id -u)" -ne 0 ]; then
    echo "Error: --tether pins the socket with -b (root), but AAudio does not render as root." >&2
    echo "For AAudio over USB tethering: enable 'USB tethering' in the phone Settings," >&2
    echo "then run:  ./scripts/termux_run.sh -s <PC-USB-IP> -d aaudio" >&2
    echo "(the server prints its USB interface IP on startup; no root needed)." >&2
    exit 1
fi

# Same for the Termux:API backend: it must run as the Termux app user (the
# API app only accepts the Termux uid and reads the segment files from the
# Termux home), so it cannot be combined with the root rndis0 pin either.
if [ "$TETHER" -eq 1 ] && [ "$IS_TERMUX" -eq 1 ] && [ "$(id -u)" -ne 0 ]; then
    echo "Error: --tether pins the socket with -b (root), but the Termux:API backend must" >&2
    echo "run as the Termux app user. For Termux:API over USB tethering: enable 'USB" >&2
    echo "tethering' in the phone Settings, then run:" >&2
    echo "  ./scripts/termux_run.sh -s <PC-USB-IP> -d termux" >&2
    echo "(the server prints its USB interface IP on startup; no root needed)." >&2
    exit 1
fi

# Direct launch (no su): in tethering mode the USB function must be restored
# afterwards, so run as a child instead of exec-ing over the script.
launch_direct() {
    if [ "$TETHER" -eq 1 ]; then
        trap '' INT   # the client (same process group) handles Ctrl+C itself
        "$ABS_BIN" "${FINAL_ARGS[@]}"
        exit $?
    else
        exec "$ABS_BIN" "${FINAL_ARGS[@]}"
    fi
}

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
    if { [ "$IS_AAUDIO" -eq 1 ] || [ "$IS_TERMUX" -eq 1 ]; } && [ "$HAS_BIND" -eq 1 ]; then
        # Root for the socket binding; AAudio runs in-process like the
        # stream_daemon (which works as root). Termux:API via su is best
        # effort: the API app still has to read the segment files and receive
        # the broadcast (the socket protocol degrades to plain am broadcast).
        if ! command -v su >/dev/null 2>&1; then
            echo "Error: '-b' needs root but 'su' is not available. Run 'su' first or install su." >&2
            exit 1
        fi
        if [ "$IS_TERMUX" -eq 1 ]; then
            echo "Note: -d termux with -b runs via su; running as the current user (no -b)"
            echo "is recommended for this backend."
        fi
        echo "Requesting root privileges via su (for -b auto); the backend runs in-process..."
        run_via_su "$CMD"
    elif [ "$IS_TERMUX" -eq 1 ]; then
        echo "Termux:API backend: running as the current user (requires the Termux:API app)."
        echo "Note: if the Termux:API media player is unavailable, the client falls back to"
        echo "AAudio (no root) and then the root backends (AGM/ALSA)."
        launch_direct
    elif [ "$IS_AAUDIO" -eq 1 ]; then
        echo "AAudio backend: running as the current user (like: ./stream_daemon)."
        echo "Note: if AAudio does not work on this device, the Termux:API / AGM / ALSA"
        echo "fallbacks take over - re-run with '-b auto' (or '-d agm') via su for the"
        echo "root backends in that case."
        launch_direct
    else
        if ! command -v su >/dev/null 2>&1; then
            echo "Error: not running as root and 'su' is not available. Run 'su' first or install su." >&2
            exit 1
        fi
        echo "Requesting root privileges via su..."
        run_via_su "$CMD"
    fi
else
    # Already root (Android/system shell): launch the absolute path directly.
    # AAudio runs in-process like the stream_daemon (which works as root);
    # Termux:API degrades to am broadcast (its socket protocol only accepts
    # the Termux app user) and logs a hint - see termux_api_player.cpp.
    launch_direct
fi
