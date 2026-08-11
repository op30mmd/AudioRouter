#!/usr/bin/env bash
# AudioRouter - Android Audio Diagnostics (Professional)
# Comprehensive diagnosis of ALSA, AGM, mixer, permissions, and network for rooted Android in Termux.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'; BLUE='\033[0;34m'; NC='\033[0m'
ok()   { echo -e "${GREEN}[OK]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
fail() { echo -e "${RED}[FAIL]${NC} $*"; }
info() { echo -e "${BLUE}[INFO]${NC} $*"; }
header() { echo ""; echo "============================================================"; echo " $*"; echo "============================================================"; }

header "AudioRouter - Android Audio Diagnostics"
echo "Date: $(date)"
echo "Project: $PROJECT_ROOT"
echo "User: $(id 2>&1)"
echo "Hostname: $(hostname 2>/dev/null || getprop ro.product.model 2>/dev/null || echo unknown)"

# Root check
header "Root and Permissions"
if [[ "$(id -u)" == "0" ]]; then
    ok "Running as root (uid=0)"
else
    warn "Not running as root (uid=$(id -u)). Many checks will fail. Run: su"
    if command -v su >/dev/null 2>&1; then
        info "su binary found, try: su -c \"$0\""
    else
        fail "su not found - device may not be rooted"
    fi
fi

# Find tinymix with fallback
find_tinymix() {
    for p in tinymix /vendor/bin/tinymix /system/bin/tinymix /data/data/com.termux/files/usr/bin/tinymix; do
        if command -v "$p" >/dev/null 2>&1 || [[ -x "$p" ]]; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

TINYMIX=""
if TINYMIX="$(find_tinymix)"; then
    ok "tinymix found: $TINYMIX"
else
    warn "tinymix not found - install alsa-utils or check /vendor/bin"
fi

# Find agmplay
find_agmplay() {
    for p in agmplay /vendor/bin/agmplay /system/bin/agmplay; do
        if command -v "$p" >/dev/null 2>&1 || [[ -x "$p" ]]; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

if AGMPLAY="$(find_agmplay)"; then
    ok "agmplay found: $AGMPLAY (Qualcomm AGM backend available)"
else
    warn "agmplay not found - AGM backend not available (normal on non-Qualcomm devices)"
fi

# /dev/snd checks
header "/dev/snd PCM Nodes"
if [[ -d /dev/snd ]]; then
    ok "/dev/snd directory exists"
    echo ""
    ls -l /dev/snd/ 2>&1 || warn "Cannot list /dev/snd/"
    echo ""
    info "Checking PCM node permissions..."
    for node in /dev/snd/*; do
        if [[ -e "$node" ]]; then
            perm="$(ls -l "$node" | awk '{print $1, $3, $4}')"
            if [[ -r "$node" && -w "$node" ]]; then
                ok "$node $perm - readable/writable"
            else
                warn "$node $perm - NOT writable (need chmod 666 $node)"
            fi
        fi
    done

    # Count playback nodes
    p_count="$(ls /dev/snd/pcm*C*D*p 2>/dev/null | wc -l)"
    info "Found $p_count playback PCM nodes (pcmC*D*p)"
    if [[ "$p_count" -eq 0 ]]; then
        warn "No playback PCM nodes found - unusual, check kernel config"
    fi
else
    fail "/dev/snd does not exist - ALSA not exposed by kernel"
fi

# /proc/asound
header "/proc/asound"
if [[ -f /proc/asound/cards ]]; then
    ok "/proc/asound/cards:"
    cat /proc/asound/cards
else
    warn "/proc/asound/cards missing"
fi

if [[ -f /proc/asound/devices ]]; then
    info "/proc/asound/devices:"
    cat /proc/asound/devices
fi

if [[ -d /proc/asound ]]; then
    info "Cards directory listing:"
    ls -R /proc/asound 2>&1 | head -n 100 || true
fi

# SoC detection
header "SoC and Hardware"
info "CPU info:"
cat /proc/cpuinfo 2>&1 | head -n 50 || true

if [[ -f /proc/asound/cards ]]; then
    if grep -qi "qualcomm\|qcom" /proc/asound/cards; then
        ok "Qualcomm audio card detected"
    fi
fi

if command -v getprop >/dev/null 2>&1; then
    info "Relevant Android properties:"
    getprop | grep -i -E "audio|qualcomm|qcom|mediatek|mtk|board|platform|snd|agm" | head -n 50 || true
fi

# Mixer controls
header "Mixer Controls (tinymix)"
if [[ -n "$TINYMIX" ]]; then
    info "All mixer controls (first 300 lines):"
    $TINYMIX 2>&1 | head -n 300 || warn "tinymix listing failed"
    echo ""
    info "Speaker-related controls:"
    $TINYMIX 2>&1 | grep -i -E "speaker|spk|rx.*volume|speaker.*function|multi.*media" -i | head -n 100 || info "No speaker controls matched filter"
else
    warn "tinymix not available, skipping mixer dump"
fi

# Client binary device enumeration
header "Client Binary Device Enumeration"
find_binary() {
    for c in "$PROJECT_ROOT/bin/audiorouter_client" "$SCRIPT_DIR/audiorouter_client" "$PROJECT_ROOT/audiorouter_client" audiorouter_client; do
        if [[ -f "$c" ]]; then echo "$c"; return 0; fi
        if command -v "$c" >/dev/null 2>&1; then command -v "$c"; return 0; fi
    done
    return 1
}

BIN=""
if BIN="$(find_binary)"; then
    ok "Client binary found: $BIN"
    if [[ -x "$BIN" ]]; then
        info "Running $BIN --list-devices"
        "$BIN" --list-devices 2>&1 || warn "Client --list-devices failed"
    else
        warn "$BIN not executable, attempting chmod +x"
        chmod +x "$BIN" 2>/dev/null || true
        "$BIN" --list-devices 2>&1 || true
    fi
else
    warn "Client binary audiorouter_client not found"
    info "Build with: ./scripts/termux_setup.sh or ./scripts/build_client.sh or ./scripts/build_all.sh"
fi

# libasound check
header "ALSA Library (libasound)"
for path in /vendor/lib64/libasound.so /vendor/lib/libasound.so /system/lib64/libasound.so /data/data/com.termux/files/usr/lib/libasound.so $PREFIX/lib/libasound.so /usr/lib/libasound.so; do
    if [[ -f "$path" ]]; then
        ok "libasound found at $path"
        ls -lh "$path" || true
    fi
done

if command -v ldd >/dev/null 2>&1 && [[ -n "$BIN" && -f "$BIN" ]]; then
    info "ldd on client binary:"
    ldd "$BIN" 2>&1 | head -n 50 || true
fi

# AGM details
header "Qualcomm AGM Details (if applicable)"
if [[ -n "${AGMPLAY:-}" ]]; then
    info "agmplay help / test:"
    $AGMPLAY --help 2>&1 | head -n 100 || $AGMPLAY -h 2>&1 | head -n 100 || true
    info "AGM config files:"
    ls -l /vendor/etc/audio/ 2>&1 | head -n 50 || ls -l /system/etc/audio* 2>&1 | head -n 50 || warn "No AGM config dir found"
fi

# Network
header "Network Interfaces and Routing"
if command -v ip >/dev/null 2>&1; then
    ip addr show 2>&1 | head -n 100 || true
    ip route show 2>&1 | head -n 50 || true
elif command -v ifconfig >/dev/null 2>&1; then
    ifconfig 2>&1 | head -n 100 || true
fi

info "Checking for VPN tunnel (tun0):"
if ip addr show tun0 2>&1 | grep -q "tun0" || ifconfig tun0 2>&1 | grep -q "tun0"; then
    warn "tun0 VPN interface detected - audio traffic may be routed through VPN"
    warn "Use client -b auto or -b wlan0 to bypass VPN"
else
    ok "No tun0 VPN interface detected (or ip/ifconfig not showing it)"
fi

# Final recommendations
header "Recommendations"

echo ""
if [[ "$(id -u)" != "0" ]]; then
    echo "- Run with root: su"
fi

if [[ -d /dev/snd ]]; then
    if [[ ! -w /dev/snd/pcmC0D0p ]]; then
        echo "- Fix permissions: chmod 666 /dev/snd/* (via su)"
    fi
fi

echo "- If no sound despite streaming:"
echo "    1. ./scripts/android_mixer_setup.sh"
echo "    2. Try devices: ./bin/audiorouter_client --list-devices"
echo "    3. Test each: -d direct:/dev/snd/pcmC0D0p, -d hw:0,0, -d agm, -d default"
echo "    4. Increase latency: -l 80"
echo "    5. Bypass VPN: -b auto"
echo ""
echo "- For detailed environment check: ./scripts/check_env.sh"
echo "- For setup: ./scripts/termux_setup.sh"
echo "- To run: ./scripts/termux_run.sh <PC_IP> --verbose"
echo ""
echo "============================================================"
echo " Diagnostics complete"
echo "============================================================"
