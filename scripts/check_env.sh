#!/usr/bin/env bash
# AudioRouter - Environment Diagnostic Script
# Checks build dependencies, audio hardware, and network on Linux / Termux / Android.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'; NC='\033[0m'
ok()   { echo -e "${GREEN}[OK]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*" >&2; }
fail() { echo -e "${RED}[FAIL]${NC} $*" >&2; }
info() { echo "[INFO] $*"; }

echo "============================================================"
echo " AudioRouter - Environment Check"
echo " Date: $(date -u)  Host: $(hostname 2>/dev/null || echo unknown)"
echo " Project: $PROJECT_ROOT"
echo "============================================================"
echo ""

echo "--- System ---"
info "OS: $(uname -a 2>/dev/null || echo unknown)"
info "Architecture: $(uname -m 2>/dev/null || arch 2>/dev/null || echo unknown)"
if [[ -f /etc/os-release ]]; then
    cat /etc/os-release | grep -E "^(NAME|VERSION|ID|PRETTY_NAME)=" || true
fi
echo ""

echo "--- Build Tools ---"
# C++ compilers
for comp in clang++ g++ aarch64-linux-gnu-g++ x86_64-linux-gnu-g++ cc c++; do
    if command -v "$comp" >/dev/null 2>&1; then
        ver="$($comp --version 2>&1 | head -n1)"
        ok "$comp: $ver"
        # Check C++23
        tmp="$(mktemp /tmp/ar_XXXX.cpp)"
        echo 'int main(){return 0;}' > "$tmp"
        if "$comp" -std=c++23 -c "$tmp" -o /tmp/ar_test.o >/dev/null 2>&1; then
            ok "  -> C++23 support: yes"
        else
            warn "  -> C++23 support: no (need GCC 13+ / Clang 16+)"
        fi
        rm -f "$tmp" /tmp/ar_test.o
    fi
done

if command -v cmake >/dev/null 2>&1; then
    ok "cmake: $(cmake --version | head -n1)"
    if cmake --version 2>&1 | grep -qE "[3-9]\.(2[0-9]|[3-9][0-9])"; then
        ok "  -> CMake >= 3.20: yes"
    else
        warn "  -> CMake < 3.20, may still work but 3.20+ recommended"
    fi
else
    warn "cmake not found (optional, but needed for MSVC and preferred for Linux)"
fi

if command -v make >/dev/null 2>&1; then
    ok "make: $(make --version 2>&1 | head -n1)"
else
    fail "make not found - required for Makefile build"
fi

if command -v pkg-config >/dev/null 2>&1; then
    ok "pkg-config: $(pkg-config --version)"
else
    warn "pkg-config not found"
fi

if command -v nproc >/dev/null 2>&1; then
    info "CPU cores: $(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
    info "CPU cores: $(sysctl -n hw.ncpu 2>/dev/null || echo unknown)"
fi

echo ""
echo "--- Libraries ---"
if pkg-config --exists alsa 2>/dev/null; then
    ok "alsa-lib (pkg-config): $(pkg-config --modversion alsa)"
else
    warn "alsa-lib not found via pkg-config (required for libasound backend, but direct ALSA still works)"
    if [[ -f /usr/include/alsa/asoundlib.h ]]; then
        ok "alsa-lib header found at /usr/include/alsa/asoundlib.h"
    elif [[ -f "$PREFIX/include/alsa/asoundlib.h" ]]; then
        ok "alsa-lib header found at $PREFIX/include/alsa/asoundlib.h"
    fi
fi

if [[ -f /usr/lib/x86_64-linux-gnu/libasound.so ]] || [[ -f /usr/lib/libasound.so ]] || [[ -f "$PREFIX/lib/libasound.so" ]]; then
    ok "libasound.so found"
else
    warn "libasound.so not found in standard paths"
fi

echo ""
echo "--- Termux / Android Specific ---"
if [[ -n "${PREFIX:-}" && "$PREFIX" == *com.termux* ]]; then
    ok "Termux environment detected at $PREFIX"
    if command -v pkg >/dev/null 2>&1; then
        ok "pkg (Termux package manager) available"
    fi
else
    info "Not running inside Termux (PREFIX=$PREFIX)"
fi

if command -v su >/dev/null 2>&1; then
    ok "su found: $(command -v su)"
    if su -c "id" 2>/dev/null | grep -q "uid=0"; then
        ok "Root access via su: working (uid=0)"
    else
        warn "su exists but root test failed - root may be denied or not properly configured"
    fi
else
    warn "su not found - root access unavailable (required for ALSA hardware)"
fi

if [[ -d /dev/snd ]]; then
    ok "/dev/snd exists"
    ls -l /dev/snd/ 2>&1 | head -n 50 || true
    if [[ -w /dev/snd ]]; then
        ok "/dev/snd writable"
    else
        warn "/dev/snd not writable - will need chmod 666 /dev/snd/* via root"
    fi
else
    warn "/dev/snd does not exist - no ALSA kernel devices"
fi

if [[ -f /proc/asound/cards ]]; then
    ok "/proc/asound/cards exists:"
    cat /proc/asound/cards || true
else
    warn "/proc/asound/cards not found"
fi

for bin in tinymix tinyplay tinycap agmplay; do
    if command -v "$bin" >/dev/null 2>&1; then
        ok "$bin found: $(command -v $bin)"
    elif [[ -x "/vendor/bin/$bin" ]]; then
        ok "$bin found at /vendor/bin/$bin"
    elif [[ -x "/system/bin/$bin" ]]; then
        ok "$bin found at /system/bin/$bin"
    else
        warn "$bin not found (optional, $bin is needed for mixer setup / AGM backend)"
    fi
done

echo ""
echo "--- Network ---"
if command -v ip >/dev/null 2>&1; then
    ok "ip command available"
    ip addr show 2>&1 | grep -E "inet|mtu|state" | head -n 30 || true
elif command -v ifconfig >/dev/null 2>&1; then
    ok "ifconfig available"
    ifconfig 2>&1 | head -n 50 || true
else
    warn "ip/ifconfig not found, cannot show interfaces"
fi

# Check UDP port 44100
if command -v ss >/dev/null 2>&1; then
    info "Listening UDP sockets:"
    ss -u -l -n 2>&1 | head -n 20 || true
elif command -v netstat >/dev/null 2>&1; then
    netstat -u -l -n 2>&1 | head -n 20 || true
fi

if command -v getprop >/dev/null 2>&1; then
    info "Android properties (wifi interface):"
    getprop | grep -i -E "wifi|dhcp|wlan" | head -n 20 || true
fi

echo ""
echo "--- Project Binaries ---"
for bin in "$PROJECT_ROOT/bin/audiorouter_server" "$PROJECT_ROOT/bin/audiorouter_client" "$PROJECT_ROOT/bin/audiorouter_tests" "$PROJECT_ROOT/bin/audiorouter_server.exe" "$PROJECT_ROOT/bin/audiorouter_client.exe"; do
    if [[ -f "$bin" ]]; then
        ok "Found $bin ($(du -h "$bin" | cut -f1))"
        "$bin" --help 2>&1 | head -n 20 || true
        echo ""
    fi
done

echo ""
echo "============================================================"
echo " Summary"
echo "============================================================"
echo " If any FAIL items are related to your target platform:"
echo "   - Linux: Install build-essential, clang, cmake, alsa-lib dev: apt install build-essential clang cmake libasound2-dev"
echo "   - Termux: Run ./scripts/termux_setup.sh  or  pkg install clang make alsa-lib alsa-utils sudo"
echo "   - Android ALSA: Requires root, then ./scripts/android_mixer_setup.sh and ./scripts/android_diagnose.sh"
echo "   - Windows: Install Visual Studio 2022 with C++ workload + CMake, or MinGW-w64 GCC 13+"
echo ""
echo " Diagnostics:"
echo "   ./scripts/android_diagnose.sh   - Detailed Android audio diagnosis"
echo "   ./scripts/build_all.sh --help   - Unified build"
echo "   ./scripts/termux_run.sh --help  - Termux runner help"
echo "============================================================"
