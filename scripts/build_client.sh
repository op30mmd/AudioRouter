#!/usr/bin/env bash
# AudioRouter - Client Build Script (Linux / Android / Termux / macOS)
# Professional, argument-aware wrapper around Makefile / CMake.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.. " && pwd || cd "$SCRIPT_DIR/.." && pwd)"

# Ensure correct project root (workaround for trailing space above)
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN_DIR="$PROJECT_ROOT/bin"
BUILD_DIR="$PROJECT_ROOT/build"

COMPILER="${CXX:-}"
BUILD_TYPE="Release"
CLEAN=0
RUN_TESTS=0
VERBOSE=0
SANITIZE=""

show_help() {
    cat <<EOF
AudioRouter Client Build Script

Usage: $(basename "$0") [options]

Options:
  -h, --help          Show this help and exit
  --clean             Remove build/ and bin/ before build
  --tests             Also build and run unit tests
  --sanitize [type]   Build with sanitizers (default: address,undefined)
  --debug             Build Debug (O0 -g), implies sanitize if no type given
  --verbose           Verbose make output
  --compiler NAME     Compiler: clang++, g++, aarch64-linux-gnu-g++
  --all               Build server, client, and tests (same as make all)

Examples:
  $(basename "$0")
  $(basename "$0") --clean
  $(basename "$0") --tests --verbose
  $(basename "$0") --compiler clang++ --debug
  $(basename "$0") --all

Outputs:
  bin/audiorouter_client
  bin/audiorouter_server (with --all or if OS supports)
EOF
}

log_info()  { echo "[INFO] $*"; }
log_warn()  { echo "[WARN] $*" >&2; }
log_error() { echo "[ERROR] $*" >&2; }

# Parse args
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) show_help; exit 0 ;;
        --clean) CLEAN=1; shift ;;
        --tests|--test) RUN_TESTS=1; shift ;;
        --sanitize)
            if [[ $# -ge 2 && "${2:-}" != --* ]]; then
                SANITIZE="$2"
                shift 2
            else
                SANITIZE="address,undefined"
                shift
            fi
            ;;
        --debug) BUILD_TYPE="Debug"; SANITIZE="${SANITIZE:-address,undefined}"; shift ;;
        --verbose|-v) VERBOSE=1; shift ;;
        --compiler) COMPILER="$2"; shift 2 ;;
        --all) BUILD_ALL=1; shift ;;
        --) shift; break ;;
        -*) log_error "Unknown option: $1"; show_help; exit 1 ;;
        *) shift ;;
    esac
done

if [[ "$CLEAN" == "1" ]]; then
    log_info "Cleaning $BUILD_DIR and $BIN_DIR"
    rm -rf "$BUILD_DIR" "$BIN_DIR"
fi

mkdir -p "$BIN_DIR" "$BUILD_DIR"

# Detect compiler if not set
if [[ -z "$COMPILER" ]]; then
    if command -v clang++ >/dev/null 2>&1; then
        COMPILER="clang++"
    elif command -v g++ >/dev/null 2>&1; then
        COMPILER="g++"
    else
        log_error "No C++ compiler found (clang++ or g++ required with C++23 support)"
        exit 1
    fi
fi

log_info "Using compiler: $COMPILER"
if ! "$COMPILER" --version 2>&1 | head -n1; then
    log_warn "Could not get compiler version for $COMPILER"
fi

# Verify C++23 support (quick compile test)
TMP_TEST="$(mktemp /tmp/ar_cxx23_XXXX.cpp)"
echo 'int main(){return 0;}' > "$TMP_TEST"
if ! "$COMPILER" -std=c++23 -c "$TMP_TEST" -o /tmp/ar_test.o >/dev/null 2>&1; then
    log_error "Compiler $COMPILER does not support -std=c++23 (need GCC 13+ or Clang 16+)"
    rm -f "$TMP_TEST" /tmp/ar_test.o
    exit 1
fi
rm -f "$TMP_TEST" /tmp/ar_test.o

MAKEFLAGS_EXTRA=""
if [[ "$VERBOSE" == "1" ]]; then
    MAKEFLAGS_EXTRA="VERBOSE=1"
fi

# Build
if [[ "${BUILD_ALL:-0}" == "1" ]]; then
    log_info "Building all targets (server + client + tests)"
    if [[ -n "$SANITIZE" ]]; then
        CXX="$COMPILER" make -C "$PROJECT_ROOT" all DEBUG=1 SANITIZE="$SANITIZE" $MAKEFLAGS_EXTRA
    else
        if [[ "$BUILD_TYPE" == "Debug" ]]; then
            CXX="$COMPILER" make -C "$PROJECT_ROOT" all DEBUG=1 $MAKEFLAGS_EXTRA
        else
            CXX="$COMPILER" make -C "$PROJECT_ROOT" all $MAKEFLAGS_EXTRA
        fi
    fi
else
    log_info "Building client only"
    if [[ -n "$SANITIZE" ]]; then
        CXX="$COMPILER" make -C "$PROJECT_ROOT" client DEBUG=1 SANITIZE="$SANITIZE" $MAKEFLAGS_EXTRA
    else
        if [[ "$BUILD_TYPE" == "Debug" ]]; then
            CXX="$COMPILER" make -C "$PROJECT_ROOT" client DEBUG=1 $MAKEFLAGS_EXTRA
        else
            CXX="$COMPILER" make -C "$PROJECT_ROOT" client $MAKEFLAGS_EXTRA
        fi
    fi
fi

if [[ -f "$BIN_DIR/audiorouter_client" ]]; then
    chmod +x "$BIN_DIR/audiorouter_client"
    log_info "Client built: $BIN_DIR/audiorouter_client"
    ls -lh "$BIN_DIR/audiorouter_client"
else
    log_error "Client binary not found after build at $BIN_DIR/audiorouter_client"
    exit 1
fi

if [[ "$RUN_TESTS" == "1" ]]; then
    log_info "Building and running tests"
    if [[ -n "$SANITIZE" ]]; then
        CXX="$COMPILER" make -C "$PROJECT_ROOT" test DEBUG=1 SANITIZE="$SANITIZE" $MAKEFLAGS_EXTRA
    else
        CXX="$COMPILER" make -C "$PROJECT_ROOT" test $MAKEFLAGS_EXTRA
    fi
fi

echo ""
echo "============================================================"
echo " Build succeeded"
echo " Binary: $BIN_DIR/audiorouter_client"
echo " Compiler: $COMPILER ($BUILD_TYPE)"
echo "============================================================"
echo " Next steps:"
echo "   $BIN_DIR/audiorouter_client --list-devices"
echo "   $BIN_DIR/audiorouter_client --help"
echo "   $BIN_DIR/audiorouter_client -s <SERVER_IP> -p 44100"
echo "============================================================"
