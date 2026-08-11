#!/usr/bin/env bash
# AudioRouter - Termux Environment Setup (Professional)
# Installs dependencies and builds the Android client in Termux.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN_DIR="$PROJECT_ROOT/bin"

# Defaults
NO_BUILD=0
FORCE=0
VERBOSE=0

show_help() {
    cat <<EOF
AudioRouter Termux Setup

Usage: $(basename "$0") [options]

Options:
  -h, --help     Show this help
  --no-build     Only install dependencies, do not compile
  --force        Force reinstall / rebuild even if binary exists
  --verbose      Verbose output
  --tests        Also build and run unit tests where possible

This script:
  1. Checks Termux environment
  2. Updates pkg and installs clang, make, alsa-lib, alsa-utils, sudo, termux-tools
  3. Builds bin/audiorouter_client with clang++ (unless --no-build)

After setup, run:
  ./scripts/termux_run.sh <PC_IP>
  or
  su -c "./bin/audiorouter_client -s <PC_IP> -p 44100"

Requirements:
  Termux from F-Droid, Android device with root for actual playback.
EOF
}

log_info()  { echo "[INFO] $*"; }
log_warn()  { echo "[WARN] $*" >&2; }
log_error() { echo "[ERROR] $*" >&2; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) show_help; exit 0 ;;
        --no-build) NO_BUILD=1; shift ;;
        --force) FORCE=1; shift ;;
        --verbose|-v) VERBOSE=1; shift ;;
        --tests) RUN_TESTS=1; shift ;;
        --) shift; break ;;
        -*) log_error "Unknown option: $1"; show_help; exit 1 ;;
        *) shift ;;
    esac
done

echo "============================================================"
echo " AudioRouter - Termux Setup"
echo " Project: $PROJECT_ROOT"
echo "============================================================"

# Environment check
if [[ -z "${PREFIX:-}" ]] || [[ ! -d "${PREFIX:-}" ]]; then
    log_warn "PREFIX not set, may not be Termux. Continuing anyway."
else
    log_info "Termux detected at $PREFIX"
fi

if ! command -v pkg >/dev/null 2>&1; then
    log_error "pkg not found. Are you running in Termux? Install Termux from F-Droid."
    exit 1
fi

# Detect existing binary
find_existing_binary() {
    local candidates=(
        "$PROJECT_ROOT/bin/audiorouter_client"
        "$SCRIPT_DIR/audiorouter_client"
        "$PROJECT_ROOT/audiorouter_client"
    )
    for c in "${candidates[@]}"; do
        if [[ -f "$c" ]]; then
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

EXISTING_BIN=""
if EXISTING_BIN="$(find_existing_binary)"; then
    log_info "Existing client binary found at: $EXISTING_BIN"
    if [[ "$FORCE" == "0" && "$NO_BUILD" == "0" ]]; then
        log_info "Use --force to rebuild anyway. Skipping build check until pkg install done."
    fi
else
    log_info "No existing client binary found, will build after pkg install."
fi

# Update and install deps
log_info "Updating Termux package lists (pkg update)..."
if [[ "$VERBOSE" == "1" ]]; then
    pkg update -y
else
    pkg update -y >/dev/null 2>&1 || log_warn "pkg update failed, continuing..."
fi

log_info "Installing dependencies: clang make alsa-lib alsa-utils sudo termux-tools pkg-config"
if [[ "$VERBOSE" == "1" ]]; then
    pkg install -y clang make alsa-lib alsa-utils sudo termux-tools pkg-config || true
else
    pkg install -y clang make alsa-lib alsa-utils sudo termux-tools pkg-config >/dev/null 2>&1 || \
    pkg install -y clang make alsa-lib alsa-utils sudo >/dev/null 2>&1 || true
fi

# Verify compiler
if ! command -v clang++ >/dev/null 2>&1; then
    log_error "clang++ not found after install. Try: pkg install clang"
    exit 1
fi
log_info "Compiler version: $(clang++ --version | head -n1)"

# Build
if [[ "$NO_BUILD" == "1" ]]; then
    log_info "--no-build specified, skipping compilation."
else
    if [[ -n "$EXISTING_BIN" && "$FORCE" == "0" ]]; then
        # If binary exists and not forced, skip build (but still report)
        if [[ -f "$PROJECT_ROOT/bin/audiorouter_client" ]]; then
            log_info "Binary exists at $PROJECT_ROOT/bin/audiorouter_client, skipping build (use --force to rebuild)."
        else
            log_info "Found binary elsewhere, but bin/audiorouter_client missing. Building for project..."
            mkdir -p "$PROJECT_ROOT/bin" "$PROJECT_ROOT/build"
            CXX=clang++ make -C "$PROJECT_ROOT" client ${VERBOSE:+VERBOSE=1}
        fi
    else
        if [[ -f "$PROJECT_ROOT/Makefile" ]]; then
            log_info "Compiling AudioRouter client with clang++..."
            mkdir -p "$PROJECT_ROOT/bin" "$PROJECT_ROOT/build"
            if [[ "${RUN_TESTS:-0}" == "1" ]]; then
                CXX=clang++ make -C "$PROJECT_ROOT" all
            else
                CXX=clang++ make -C "$PROJECT_ROOT" client
            fi
        else
            log_warn "Makefile not found at $PROJECT_ROOT/Makefile, cannot build."
            log_warn "Place pre-compiled 'audiorouter_client' in bin/ or project root."
        fi
    fi
fi

FINAL_BIN=""
if FINAL_BIN="$(find_existing_binary)"; then
    chmod +x "$FINAL_BIN" 2>/dev/null || true
    log_info "Final binary: $FINAL_BIN"
    ls -lh "$FINAL_BIN" 2>/dev/null || true
else
    log_warn "No client binary found after setup. You may need to build manually or download release tar.gz"
fi

echo ""
echo "============================================================"
echo " Setup complete"
echo "============================================================"
echo " Installed packages: clang, make, alsa-lib, alsa-utils, sudo"
echo ""
echo " To diagnose Android audio environment:"
echo "   ./scripts/android_diagnose.sh"
echo "   ./scripts/check_env.sh"
echo ""
echo " To run with auto mixer setup (requires root):"
echo "   su"
echo "   ./scripts/android_mixer_setup.sh"
echo "   ./bin/audiorouter_client -s <PC_IP> -p 44100"
echo ""
echo " Or use automated runner:"
echo "   ./scripts/termux_run.sh <PC_IP> [options]"
echo "   ./scripts/termux_run.sh --help"
echo "============================================================"
