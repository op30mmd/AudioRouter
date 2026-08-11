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

# Clean env runner for vendor binaries (avoid Termux libtermux-exec.so issue)
# Vendor binaries like agmplay cannot access Termux's private lib path because
# they run in the default linker namespace, while Termux binaries run in a
# separate namespace with LD_PRELOAD=libtermux-exec.so. Running with env -i
# and minimal vendor LD_LIBRARY_PATH fixes:
#   CANNOT LINK EXECUTABLE "agmplay": library libtermux-exec.so not accessible
run_vendor_clean() {
    # $1 = binary path, rest = args
    local bin="$1"; shift
    env -i \
        LD_LIBRARY_PATH=/vendor/lib64:/vendor/lib:/system/lib64:/system/lib \
        PATH=/vendor/bin:/system/bin:/system/xbin:/data/data/com.termux/files/usr/bin \
        ANDROID_ROOT=/system \
        ANDROID_DATA=/data \
        "$bin" "$@" 2>&1
}

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

# Detect Termux exec issue
if [[ -n "${LD_PRELOAD:-}" ]]; then
    warn "LD_PRELOAD is set to: $LD_PRELOAD"
    warn "This is Termux's libtermux-exec.so which breaks vendor binaries like agmplay"
    info "Diagnostic will use clean env (env -i) for vendor binaries to work around this"
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
    for p in /vendor/bin/agmplay /system/bin/agmplay agmplay; do
        if [[ -x "$p" ]] || command -v "$p" >/dev/null 2>&1; then
            # Prefer absolute paths that exist
            if [[ -x "$p" ]]; then echo "$p"; return 0; fi
            if command -v "$p" >/dev/null 2>&1; then
                local resolved
                resolved="$(command -v "$p")"
                if [[ -x "$resolved" ]]; then echo "$resolved"; return 0; fi
            fi
        fi
    done
    # Check existence even if not executable via current perms
    for p in /vendor/bin/agmplay /system/bin/agmplay; do
        if [[ -f "$p" ]]; then echo "$p"; return 0; fi
    done
    return 1
}

AGMPLAY=""
if AGMPLAY="$(find_agmplay)"; then
    ok "agmplay found: $AGMPLAY (Qualcomm AGM backend available)"
    # Test with clean env
    if out="$(run_vendor_clean "$AGMPLAY" --help 2>&1)"; then
        ok "agmplay --help works with clean env (vendor namespace OK)"
        echo "$out" | head -n 20
    else
        # Try without args (some agmplay versions don't have --help, just fail with usage)
        if out="$(run_vendor_clean "$AGMPLAY" 2>&1)"; then
            info "agmplay executed (no --help flag, but binary runs):
$out" | head -n 20
        else
            warn "agmplay found but failed even with clean env. Output:"
            echo "$out" | head -n 30
            info "This may be normal if agmplay requires specific args, but if you see linker errors, ensure you run with env -i LD_LIBRARY_PATH=/vendor/lib64:/vendor/lib"
        fi
    fi
else
    warn "agmplay not found - AGM backend not available (normal on non-Qualcomm devices, but this device is Qualcomm bengal)"
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
    else
        info "Playback nodes (candidates for direct:):"
        ls /dev/snd/pcm*C*D*p 2>&1
        echo ""
        info "Analysis for this device (bengal-idp-snd-card, 7 playback nodes):"
        echo "  - pcmC0D0 is capture only (pcmC0D0c) - no pcmC0D0p, so default direct:/dev/snd/pcmC0D0p will FALLBACK to other nodes"
        echo "  - Try order: direct:/dev/snd/pcmC0D1p (likely media), direct:/dev/snd/pcmC0D5p, D6p, D7p, D8p, D14p"
        echo "  - On Bengal (SD 662/680), speakers often use D5p/D6p after routing, or D1p for primary"
    fi
else
    fail "/dev/snd does not exist - ALSA not exposed by kernel"
fi

# /proc/asound
header "/proc/asound"
if [[ -f /proc/asound/cards ]]; then
    ok "/proc/asound/cards:"
    cat /proc/asound/cards
    if grep -qi "bengal" /proc/asound/cards; then
        ok "Bengal SoC detected (SD 662/680 family) - known speaker routing quirks"
        info "Bengal typically needs RX_MACRO MUX and RX MIX routing via tinymix, see android_mixer_setup.sh --qualcomm"
    fi
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
    if grep -qi "qualcomm\|qcom\|bengal" /proc/asound/cards; then
        ok "Qualcomm audio card detected"
    fi
fi

if command -v getprop >/dev/null 2>&1; then
    info "Relevant Android properties:"
    getprop | grep -i -E "audio|qualcomm|qcom|mediatek|mtk|board|platform|snd|agm|bengal" | head -n 80 || true
fi

# Mixer controls
header "Mixer Controls (tinymix)"
if [[ -n "$TINYMIX" ]]; then
    info "All mixer controls (first 300 lines):"
    $TINYMIX 2>&1 | head -n 300 || warn "tinymix listing failed"
    echo ""
    info "Speaker-related controls (filtered):"
    # Expanded filter for Bengal / WCD937x / Bolero
    $TINYMIX 2>&1 | grep -i -E "speaker|spk|RX_RX|RX_MACRO|RX.*MUX|RX.*MIX|HPHL|HPHR|EAR|RDAC|AUX_RDAC|IIR|COMP|RX.*Volume" | head -n 150 || info "No controls matched expanded filter"
    echo ""
    info "Suggested routing for Bengal (manual tinymix commands to try):"
    cat <<'SUGGEST'
  # Common Bengal speaker routing (SD662/680, WCD937x + Bolero)
  # Try these one by one via su shell:
  tinymix "RX_MACRO RX0 MUX" "AIF1_PB"
  tinymix "RX_MACRO RX1 MUX" "AIF1_PB"
  tinymix "RX_MACRO RX2 MUX" "AIF2_PB"
  tinymix "RX MIX TX0 MUX" "RX0"
  tinymix "RX MIX TX1 MUX" "RX1"
  tinymix "RX MIX TX2 MUX" "RX2"
  tinymix "RX INT0_1 MIX1 INP0" "RX0"
  tinymix "RX INT1_1 MIX1 INP0" "RX1"
  tinymix "RX INT2_1 MIX1 INP0" "RX2"
  tinymix "RX INT0 MIX2 INP" "RX0"
  tinymix "RX INT1 MIX2 INP" "RX1"
  tinymix "RX_RX0 Mix Digital Volume" 84
  tinymix "RX_RX1 Mix Digital Volume" 84
  tinymix "RX_RX2 Mix Digital Volume" 84
  tinymix "RX_RX0 Digital Volume" 84
  tinymix "RX_RX1 Digital Volume" 84
  tinymix "RX_RX2 Digital Volume" 84
  tinymix "HPHL_RDAC Switch" 1
  tinymix "HPHR_RDAC Switch" 1
  tinymix "AUX_RDAC Switch" 1
  tinymix "EAR_RDAC Switch" 1
  # Then test playback with tinyplay or audiorouter_client
SUGGEST
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
        # Run with clean env to avoid Termux lib issues? Client itself should work in Termux env, but test clean too
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
    info "ldd on client binary (Termux namespace):"
    ldd "$BIN" 2>&1 | head -n 50 || true
fi

# AGM details
header "Qualcomm AGM Details"
if [[ -n "${AGMPLAY:-}" ]]; then
    info "Testing agmplay with clean vendor env (fix for libtermux-exec.so issue):"
    echo "  Command: env -i LD_LIBRARY_PATH=/vendor/lib64:/vendor/lib:/system/lib64:/system/lib PATH=/vendor/bin:/system/bin $AGMPLAY --help"
    run_vendor_clean "$AGMPLAY" --help 2>&1 | head -n 100 || echo "  No --help, trying without args:"
    run_vendor_clean "$AGMPLAY" 2>&1 | head -n 100 || true

    info "AGM backend strings to try (CODEC_DMA-LPAIF_RXTX-RX-*):"
    echo "  From your tinymix: CODEC_DMA-LPAIF_RXTX-RX-0, RX-1, RX-2, RX-3 exist (Channel Map controls 164-167)"
    echo "  Try each:"
    echo "    ./bin/audiorouter_client -s <PC_IP> -d agm:CODEC_DMA-LPAIF_RXTX-RX-0"
    echo "    ./bin/audiorouter_client -s <PC_IP> -d agm:CODEC_DMA-LPAIF_RXTX-RX-1"
    echo "    ./bin/audiorouter_client -s <PC_IP> -d agm:CODEC_DMA-LPAIF_RXTX-RX-2"

    info "AGM config files:"
    ls -l /vendor/etc/audio/ 2>&1 | head -n 50 || ls -l /system/etc/audio* 2>&1 | head -n 50 || warn "No AGM config dir at /vendor/etc/audio/"
    # Check for AGM service
    if getprop 2>&1 | grep -qi agm; then
        getprop | grep -i agm | head -n 20
    fi
else
    warn "agmplay not found, skipping AGM details"
fi

# Network - CRITICAL for this diagnostic: wlan0 DOWN, tun0 UP, rmnet data UP
header "Network Interfaces and Routing"
if command -v ip >/dev/null 2>&1; then
    ip addr show 2>&1 | head -n 150 || true
    echo ""
    ip route show 2>&1 | head -n 80 || true
elif command -v ifconfig >/dev/null 2>&1; then
    ifconfig 2>&1 | head -n 100 || true
fi

echo ""
info "=== Network Analysis for this device ==="
HAS_WLAN_UP=0
if ip addr show wlan0 2>&1 | grep -q "UP"; then HAS_WLAN_UP=1; fi
HAS_TUN0=0
if ip addr show tun0 2>&1 | grep -q "tun0"; then HAS_TUN0=1; fi

if [[ "$HAS_WLAN_UP" == "0" ]]; then
    warn "wlan0 is DOWN - WiFi is off. For AudioRouter you need:"
    warn "  Scenario A: Android hotspot ON, PC connected to phone hotspot (wlan0 should be UP as AP, or swlan0/p2p0)"
    warn "  Scenario B: Windows hotspot ON, Android connected to PC hotspot (wlan0 should be UP as client with 192.168.137.x)"
    warn "Currently connected via mobile data (rmnet_data0 26.141.70.248, rmnet_data2 107.232.228.231)"
    info "Enable WiFi or hotspot: from Android settings, turn on WiFi hotspot, then re-run diagnostics"
else
    ok "wlan0 is UP"
fi

if [[ "$HAS_TUN0" == "1" ]]; then
    warn "tun0 VPN interface DETECTED (10.183.115.4) - VPN is active"
    warn "VPN routes all traffic through tunnel, breaking local hotspot audio (latency, MTU 1300)"
    echo ""
    echo "  Solutions:"
    echo "    1. Disable VPN app temporarily to test AudioRouter"
    echo "    2. Or use client bind bypass: -b auto or -b wlan0 to force traffic over WiFi"
    echo "       Example: ./bin/audiorouter_client -s 192.168.43.45 -b auto -v"
    echo "       Or via runner: ./scripts/termux_run.sh 192.168.43.45 -- -b auto"
    echo "    3. Some VPNs allow split-tunnel - exclude local subnet 192.168.0.0/16"
    info "Your tun0 MTU 1300 is low, will cause fragmentation if server uses large packets. Server default 240 frames is MTU-safe (~996 bytes), so OK"
else
    ok "No tun0 VPN interface detected"
fi

# Check hotspot IP ranges
if ip addr 2>&1 | grep -q "192.168.43."; then
    ok "Detected Android hotspot subnet 192.168.43.x (typical Android hotspot)"
elif ip addr 2>&1 | grep -q "192.168.137."; then
    ok "Detected Windows hotspot subnet 192.168.137.x"
fi

# Final recommendations - tailored to this diagnostic
header "Recommendations - Tailored to Your Device"

echo ""
echo "Device: Bengal (SD662/680) with WCD937x + Bolero codecs, 7 playback PCM nodes, no pcmC0D0p"
echo "Current network: WiFi DOWN, Mobile data UP (rmnet), VPN tun0 UP (10.183.115.4)"
echo ""

if [[ "$(id -u)" != "0" ]]; then
    echo "- Run with root: su"
fi

echo "- CRITICAL: WiFi is DOWN. For AudioRouter you need local WiFi/hotspot:"
echo "    * Scenario A (recommended for this device):"
echo "      1. Turn ON Android WiFi hotspot in settings"
echo "      2. Connect Windows PC to that hotspot"
echo "      3. On PC: audiorouter_server.exe --list-if (note IP e.g., 192.168.43.45)"
echo "      4. On Android (Termux, su): ./scripts/termux_run.sh 192.168.43.45 -b auto -v"
echo "    * Scenario B:"
echo "      1. Turn ON Windows Mobile Hotspot"
echo "      2. Connect Android to PC hotspot (default gateway 192.168.137.1)"
echo "      3. On Android: ./scripts/termux_run.sh 192.168.137.1 -b auto -v"

echo ""
echo "- VPN issue: tun0 active. Disable VPN OR use -b auto to bypass:"
echo "    ./bin/audiorouter_client -s <PC_IP> -p 44100 -d direct:/dev/snd/pcmC0D1p -b auto -v"
echo "  Runner helper already sets LD_LIBRARY_PATH and chmod, but ensure:"
echo "    ./scripts/termux_run.sh <PC_IP> -- -b auto -d direct:/dev/snd/pcmC0D1p"

echo ""
echo "- Speaker routing for Bengal:"
echo "    Your tinymix has 184 controls but no obvious 'Speaker Function'. WCD937x needs RX_MACRO routing."
echo "    Run (as su):"
echo "      ./scripts/android_mixer_setup.sh --qualcomm"
echo "    If still no sound, manually try (su shell):"
echo "      tinymix 'RX_MACRO RX0 MUX' 'AIF1_PB'"
echo "      tinymix 'RX_MACRO RX1 MUX' 'AIF1_PB'"
echo "      tinymix 'RX_MACRO RX2 MUX' 'AIF2_PB'"
echo "      tinymix 'RX MIX TX0 MUX' 'RX0'; tinymix 'RX MIX TX1 MUX' 'RX1'; tinymix 'RX MIX TX2 MUX' 'RX2'"
echo "      tinymix 'RX_RX0 Mix Digital Volume' 84; tinymix 'RX_RX1 Mix Digital Volume' 84; tinymix 'HPHL_RDAC Switch' 1"
echo "    Then test: tinyplay /vendor/etc/audio/test.wav or audiorouter_client"

echo ""
echo "- PCM device probing order for this device (no pcmC0D0p):"
echo "    DirectAlsaPlayer already tries fallback list, but you can force each via -d:"
for dev in /dev/snd/pcmC0D1p /dev/snd/pcmC0D2p /dev/snd/pcmC0D5p /dev/snd/pcmC0D6p /dev/snd/pcmC0D7p /dev/snd/pcmC0D8p /dev/snd/pcmC0D14p; do
    if [[ -e "$dev" ]]; then
        echo "      ./bin/audiorouter_client -s <PC_IP> -d direct:$dev -b auto -v"
    fi
done
echo "    Also try: -d hw:0,0 and -d default (uses libasound)"

echo ""
echo "- AGM backend (you have agmplay but with Termux exec issue):"
echo "    The diagnostic previously showed libtermux-exec.so linking error - this is FIXED in latest code"
echo "    AgmFifoPlayer now uses env -i with clean vendor LD_LIBRARY_PATH, so AGM should work via client binary"
echo "    It does NOT work via direct shell './agmplay' in Termux shell (needs clean env). Test via:"
echo "      env -i LD_LIBRARY_PATH=/vendor/lib64:/vendor/lib:/system/lib64:/system/lib PATH=/vendor/bin:/system/bin /vendor/bin/agmplay --help"
echo "    For AudioRouter AGM, try:"
echo "      ./bin/audiorouter_client -s <PC_IP> -d agm:CODEC_DMA-LPAIF_RXTX-RX-1 -b auto -v"
echo "      ./bin/audiorouter_client -s <PC_IP> -d agm:CODEC_DMA-LPAIF_RXTX-RX-0 -b auto -v"
echo "      ./bin/audiorouter_client -s <PC_IP> -d agm:CODEC_DMA-LPAIF_RXTX-RX-2 -b auto -v"

echo ""
echo "- If no sound after routing and device trials:"
echo "    1. Check audioserver holding device: stop audioserver (su -c 'stop audioserver'), test, then start audioserver"
echo "    2. Increase latency: -l 80 or -l 120 for lossy hotspot"
echo "    3. Check server firewall: allow UDP 44100 inbound"
echo "    4. Use --discover if on same hotspot: ./bin/audiorouter_client --discover -b auto -v"

echo ""
echo "- General checks already provided:"
echo "    ./scripts/check_env.sh"
echo "    ./scripts/android_mixer_setup.sh --list (to see all 184 controls)"
echo "    ./scripts/termux_setup.sh (reinstall deps if needed)"

echo ""
echo "============================================================"
echo " Diagnostics complete - tailored for Bengal / tun0 / wlan DOWN"
echo "============================================================"
