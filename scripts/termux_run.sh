#!/usr/bin/env bash
# AudioRouter - Termux Client Runner (Professional)
# Wrapper that handles binary discovery, permission fixes, library path setup,
# and user-friendly CLI for the Android ALSA client.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Default values
SERVER_IP=""
PORT="44100"
DEVICE="default"
LATENCY=""
BIND_IFACE=""
AUTO_DISCOVER=0
VERBOSE=0
EXTRA_ARGS=()

show_help() {
    cat <<EOF
AudioRouter Termux Runner

Usage: $(basename "$0") [options] [SERVER_IP] [PORT]

This script automatically:
  - Finds or builds bin/audiorouter_client
  - Fixes /dev/snd/* permissions (requires root via su)
  - Sets LD_LIBRARY_PATH for vendor libraries (AGM support)
  - Runs the client with provided options

Arguments:
  SERVER_IP           Windows PC IP (e.g., 192.168.43.45 or 192.168.137.1)
  PORT                Server UDP port (default: 44100)

Options:
  -s, --server IP     Server IP (same as positional arg)
  -p, --port PORT     Server port (default: 44100)
  -d, --device DEV    ALSA device: default, hw:0,0, direct:/dev/snd/pcmC0D0p, agm, agm:<backend>
  -l, --latency MS    Target jitter buffer latency in ms (default: 35)
  -b, --bind IFACE    Bind to interface to bypass VPN tunnel: auto, wlan0, etc.
  --discover          Auto-discover server on hotspot subnet
  --verbose, -v       Enable verbose logging for client
  --dummy             Use dummy player (test without audio hardware)
  --list-devices      List ALSA devices and exit (no server needed)
  -h, --help          Show this help and exit

Examples:
  $(basename "$0") 192.168.43.45
  $(basename "$0") -s 192.168.43.45 -p 44100 -d direct:/dev/snd/pcmC0D0p
  $(basename "$0") -s 192.168.43.45 -l 80 -b auto
  $(basename "$0") --discover --verbose
  $(basename "$0") --list-devices

Notes:
  - Root is required for ALSA hardware access. Script will attempt su automatically.
  - If no IP is given and --discover is not used, script prompts interactively.
  - Run android_mixer_setup.sh if you hear no sound despite streaming:
      su -c ./scripts/android_mixer_setup.sh
EOF
}

log_info()  { echo "[INFO] $*"; }
log_warn()  { echo "[WARN] $*" >&2; }
log_error() { echo "[ERROR] $*" >&2; }

find_binary() {
    local candidates=(
        "$PROJECT_ROOT/bin/audiorouter_client"
        "$SCRIPT_DIR/audiorouter_client"
        "$PROJECT_ROOT/audiorouter_client"
    )
    for c in "${candidates[@]}"; do
        if [[ -f "$c" && -x "$c" ]]; then
            echo "$c"
            return 0
        elif [[ -f "$c" ]]; then
            echo "$c"
            return 0
        fi
    done
    if command -v audiorouter_client >/dev/null 2>&1; then
        command -v audiorouter_client
        return 0
    fi
    return 1
}

build_if_missing() {
    if [[ -f "$PROJECT_ROOT/Makefile" ]]; then
        log_info "Client binary not found, attempting build with clang++..."
        mkdir -p "$PROJECT_ROOT/bin" "$PROJECT_ROOT/build"
        if command -v clang++ >/dev/null 2>&1; then
            CXX=clang++ make -C "$PROJECT_ROOT" client
        else
            make -C "$PROJECT_ROOT" client
        fi
    fi
}

# Parse options
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) show_help; exit 0 ;;
        -s|--server) SERVER_IP="$2"; shift 2 ;;
        -p|--port) PORT="$2"; shift 2 ;;
        -d|--device) DEVICE="$2"; shift 2 ;;
        -l|--latency) LATENCY="$2"; shift 2 ;;
        -b|--bind) BIND_IFACE="$2"; shift 2 ;;
        --discover) AUTO_DISCOVER=1; shift ;;
        --verbose|-v) VERBOSE=1; EXTRA_ARGS+=("-v"); shift ;;
        --dummy) EXTRA_ARGS+=("--dummy"); shift ;;
        --list-devices) EXTRA_ARGS+=("--list-devices"); shift ;;
        --) shift; EXTRA_ARGS+=("$@"); break ;;
        -*) log_error "Unknown option: $1"; show_help; exit 1 ;;
        *)
            # Positional: first is server IP, second is port if numeric
            if [[ -z "$SERVER_IP" ]]; then
                SERVER_IP="$1"
            elif [[ "$PORT" == "44100" && "$1" =~ ^[0-9]+$ ]]; then
                PORT="$1"
            else
                EXTRA_ARGS+=("$1")
            fi
            shift
            ;;
    esac
done

# Locate binary
BIN_PATH=""
if ! BIN_PATH="$(find_binary)"; then
    build_if_missing
    if ! BIN_PATH="$(find_binary)"; then
        log_error "Client binary not found and automatic build failed."
        log_error "Place pre-compiled 'audiorouter_client' in bin/ or run termux_setup.sh"
        exit 1
    fi
fi

chmod +x "$BIN_PATH" 2>/dev/null || true
log_info "Using binary: $BIN_PATH"

# Special mode: --list-devices does not need server IP
NEEDS_SERVER=1
for arg in "${EXTRA_ARGS[@]}"; do
    if [[ "$arg" == "--list-devices" ]]; then
        NEEDS_SERVER=0
    fi
done

if [[ "$NEEDS_SERVER" == "1" && -z "$SERVER_IP" && "$AUTO_DISCOVER" == "0" ]]; then
    echo "============================================================"
    echo " AudioRouter Android ALSA Client"
    echo "============================================================"
    echo ""
    echo "Enter Windows PC IP address (e.g., 192.168.43.45 or 192.168.137.1)"
    echo "Or press Enter to use auto-discovery:"
    read -r SERVER_IP || true
    if [[ -z "$SERVER_IP" ]]; then
        AUTO_DISCOVER=1
        log_info "No IP entered, switching to --discover mode"
    fi
fi

# Build client args
CLIENT_ARGS=()

if [[ "$AUTO_DISCOVER" == "1" ]]; then
    CLIENT_ARGS+=("--discover")
else
    if [[ -n "$SERVER_IP" ]]; then
        CLIENT_ARGS+=("-s" "$SERVER_IP")
    fi
fi

CLIENT_ARGS+=("-p" "$PORT")

if [[ "$DEVICE" != "default" ]]; then
    CLIENT_ARGS+=("-d" "$DEVICE")
fi

if [[ -n "$LATENCY" ]]; then
    CLIENT_ARGS+=("-l" "$LATENCY")
fi

if [[ -n "$BIND_IFACE" ]]; then
    CLIENT_ARGS+=("-b" "$BIND_IFACE")
fi

CLIENT_ARGS+=("${EXTRA_ARGS[@]}")

log_info "Server: ${SERVER_IP:-auto-discover} Port: $PORT Device: $DEVICE ${LATENCY:+Latency: ${LATENCY}ms} ${BIND_IFACE:+Bind: $BIND_IFACE}"
if [[ "$VERBOSE" == "1" ]]; then
    log_info "Full command: $BIN_PATH ${CLIENT_ARGS[*]}"
fi

# Prepare environment setup for root execution
# Include vendor lib paths for Qualcomm AGM
# IMPORTANT: Clear LD_PRELOAD (Termux's libtermux-exec.so) which breaks vendor binaries like agmplay
LD_PATH="/vendor/lib64:/vendor/lib:/system/lib64:/system/lib:${LD_LIBRARY_PATH:-}"

fix_perms() {
    chmod 666 /dev/snd/* 2>/dev/null || true
}

# Network pre-checks (VPN tun0, wlan0 down)
if command -v ip >/dev/null 2>&1; then
    if ip addr show tun0 2>&1 | grep -q "tun0"; then
        log_warn "tun0 VPN interface detected - VPN may route audio through tunnel (higher latency, MTU 1300)"
        if [[ -z "$BIND_IFACE" ]]; then
            log_warn "Consider using -b auto to bypass VPN: $0 -s $SERVER_IP -b auto"
        fi
    fi
    if ip addr show wlan0 2>&1 | grep -q "wlan0"; then
        if ! ip addr show wlan0 2>&1 | grep -q "UP"; then
            log_warn "wlan0 is DOWN - WiFi/hotspot is off. AudioRouter needs local WiFi/hotspot:"
            log_warn "  Scenario A: Android hotspot ON, PC connected to phone (192.168.43.x)"
            log_warn "  Scenario B: Windows hotspot ON, Android connected to PC (192.168.137.x)"
        fi
    fi
fi

# Validate server IP - warn if using non-local IP (like 10.x VPN IP)
if [[ -n "$SERVER_IP" ]]; then
    if [[ "$SERVER_IP" =~ ^10\. ]] || [[ "$SERVER_IP" =~ ^172\.(1[6-9]|2[0-9]|3[0-1])\. ]] || [[ "$SERVER_IP" =~ ^26\. ]] || [[ "$SERVER_IP" =~ ^107\. ]]; then
        # 10.x, 172.16-31.x could be VPN/mobile data, 26.x / 107.x seen on rmnet_data - likely carrier NAT not hotspot
        if ! [[ "$SERVER_IP" =~ ^192\.168\. ]] && ! [[ "$SERVER_IP" =~ ^172\.16\. ]] ; then
            # Check if it's 192.168.43.x or 192.168.137.x - those are hotspot defaults (good)
            if [[ "$SERVER_IP" != 192.168.43.* && "$SERVER_IP" != 192.168.137.* ]]; then
                log_warn "Server IP $SERVER_IP looks like VPN/mobile carrier IP, not hotspot (expected 192.168.43.x or 192.168.137.x)"
                log_warn "  If PC is on Android hotspot, PC IP should be 192.168.43.x (check with audiorouter_server --list-if or ipconfig)"
                log_warn "  If Android is on Windows hotspot, server IP should be 192.168.137.1"
                log_warn "  Current IP $SERVER_IP with tun0 VPN active will have high latency / packet loss"
            fi
        fi
    fi
fi

if [[ "$(id -u)" -ne 0 ]]; then
    log_info "Requesting root privileges via su..."
    if ! command -v su >/dev/null 2>&1; then
        log_warn "su not found. Running without root - ALSA access will likely fail."
        log_warn "Install root (Magisk) and ensure su is available."
        fix_perms
        LD_PRELOAD="" LD_LIBRARY_PATH="$LD_PATH" exec "$BIN_PATH" "${CLIENT_ARGS[@]}"
    else
        # Use su -c; use single quotes to avoid double expansion issues, and fix perms inside
        exec su -c "chmod 666 /dev/snd/* 2>/dev/null || true; export HOME=\${HOME:-/data/data/com.termux/files/home}; export LD_PRELOAD=\"\"; export LD_LIBRARY_PATH=$LD_PATH; export PATH=/data/data/com.termux/files/usr/bin:/vendor/bin:/system/bin:\$PATH; \"$BIN_PATH\" ${CLIENT_ARGS[*]}"
    fi
else
    log_info "Already running as root (uid=0), fixing permissions..."
    fix_perms
    export LD_PRELOAD=""
    export LD_LIBRARY_PATH="$LD_PATH"
    exec "$BIN_PATH" "${CLIENT_ARGS[@]}"
fi
