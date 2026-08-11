#!/usr/bin/env bash
# AudioRouter - Unified Build Script (Linux / Termux / macOS)
# Builds server, client, and tests with CMake or Makefile fallback.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN_DIR="$PROJECT_ROOT/bin"
BUILD_DIR="$PROJECT_ROOT/build"

COMPILER="${CXX:-}"
CONFIG="Release"
CLEAN=0
VERBOSE=0
SANITIZE=""
BUILD_SERVER=1
BUILD_CLIENT=1
BUILD_TESTS=1
USE_CMAKE=0

show_help() {
    cat <<EOF
AudioRouter Unified Build

Usage: $(basename "$0") [options]

Options:
  -h, --help          Show help
  --clean             Clean build/ and bin/ before build
  --debug             Build Debug
  --release           Build Release (default)
  --sanitize [type]   Enable sanitizers (e.g., address,undefined)
  --server-only       Only build server
  --client-only       Only build client
  --tests-only        Only build and run tests
  --no-tests          Skip tests
  --cmake             Force CMake build (default: try Makefile first, then CMake)
  --compiler NAME     C++ compiler (clang++, g++, etc.)
  -v, --verbose       Verbose output

Examples:
  $(basename "$0")
  $(basename "$0") --clean --tests-only
  $(basename "$0") --server-only --debug
  $(basename "$0") --client-only --compiler clang++
  $(basename "$0") --cmake --verbose

Outputs:
  bin/audiorouter_server (Linux)
  bin/audiorouter_client
  bin/audiorouter_tests
EOF
}

log_info()  { echo "[INFO] $*"; }
log_warn()  { echo "[WARN] $*" >&2; }
log_error() { echo "[ERROR] $*" >&2; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) show_help; exit 0 ;;
        --clean) CLEAN=1; shift ;;
        --debug) CONFIG="Debug"; shift ;;
        --release) CONFIG="Release"; shift ;;
        --sanitize)
            if [[ $# -ge 2 && "${2:-}" != --* ]]; then
                SANITIZE="$2"
                shift 2
            else
                SANITIZE="address,undefined"
                shift
            fi
            ;;
        --server-only) BUILD_CLIENT=0; BUILD_TESTS=0; shift ;;
        --client-only) BUILD_SERVER=0; BUILD_TESTS=0; shift ;;
        --tests-only) BUILD_SERVER=0; BUILD_CLIENT=0; BUILD_TESTS=1; shift ;;
        --no-tests) BUILD_TESTS=0; shift ;;
        --cmake) USE_CMAKE=1; shift ;;
        --compiler) COMPILER="$2"; shift 2 ;;
        -v|--verbose) VERBOSE=1; shift ;;
        --) shift; break ;;
        -*) log_error "Unknown option: $1"; show_help; exit 1 ;;
        *) shift ;;
    esac
done

echo "============================================================"
echo " AudioRouter - Unified Build (All Targets)"
echo " Project: $PROJECT_ROOT"
echo " Config : $CONFIG ${SANITIZE:+ Sanitize: $SANITIZE}"
echo "============================================================"

if [[ "$CLEAN" == "1" ]]; then
    log_info "Cleaning $BUILD_DIR and $BIN_DIR"
    rm -rf "$BUILD_DIR" "$BIN_DIR"
fi
mkdir -p "$BIN_DIR" "$BUILD_DIR"

# Compiler detection
if [[ -z "$COMPILER" ]]; then
    if command -v clang++ >/dev/null 2>&1; then COMPILER="clang++"
    elif command -v g++ >/dev/null 2>&1; then COMPILER="g++"
    else log_error "No compiler found (need clang++ or g++ with C++23)"; exit 1; fi
fi
log_info "Compiler: $COMPILER ($("$COMPILER" --version | head -n1))"

# Check C++23
TMP="$(mktemp /tmp/ar_XXXX.cpp)"
echo 'int main(){return 0;}' > "$TMP"
if ! "$COMPILER" -std=c++23 -c "$TMP" -o "$BUILD_DIR/cxx23_test.o" >/dev/null 2>&1; then
    log_error "$COMPILER does not support -std=c++23"
    rm -f "$TMP"
    exit 1
fi
rm -f "$TMP" "$BUILD_DIR/cxx23_test.o"

if [[ "$USE_CMAKE" == "1" ]]; then
    if ! command -v cmake >/dev/null 2>&1; then
        log_error "cmake requested but not found in PATH"
        exit 1
    fi
    log_info "Building with CMake (Build type: $CONFIG)"
    CMAKE_ARGS=(-B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$CONFIG" -DCMAKE_CXX_COMPILER="$COMPILER")
    if [[ "$CONFIG" == "Debug" && -n "$SANITIZE" ]]; then
        CMAKE_ARGS+=(-DCMAKE_CXX_FLAGS="-fsanitize=$SANITIZE -fno-omit-frame-pointer")
    fi
    cmake "${CMAKE_ARGS[@]}" "$PROJECT_ROOT"
    cmake --build "$BUILD_DIR" --config "$CONFIG" --parallel $(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    # Copy artifacts to bin/
    find "$BUILD_DIR" -type f -name "audiorouter_*" -executable -exec cp -v {} "$BIN_DIR/" \; 2>/dev/null || true
    find "$BUILD_DIR/src" -type f -name "audiorouter_*" -exec cp -v {} "$BIN_DIR/" \; 2>/dev/null || true
else
    # Use Makefile - more direct and uses hardening flags from Makefile
    log_info "Building with Makefile (CXX=$COMPILER, CONFIG=$CONFIG)"
    MAKE_EXTRA=""
    if [[ "$VERBOSE" == "1" ]]; then MAKE_EXTRA="VERBOSE=1"; fi
    DEBUG_FLAG=0
    if [[ "$CONFIG" == "Debug" ]]; then DEBUG_FLAG=1; fi

    if [[ "$BUILD_SERVER" == "1" && "$BUILD_CLIENT" == "1" && "$BUILD_TESTS" == "1" ]]; then
        if [[ -n "$SANITIZE" ]]; then
            CXX="$COMPILER" make -C "$PROJECT_ROOT" all DEBUG="$DEBUG_FLAG" SANITIZE="$SANITIZE" $MAKE_EXTRA
        else
            CXX="$COMPILER" make -C "$PROJECT_ROOT" all DEBUG="$DEBUG_FLAG" $MAKE_EXTRA
        fi
    else
        [[ "$BUILD_SERVER" == "1" ]] && CXX="$COMPILER" make -C "$PROJECT_ROOT" server DEBUG="$DEBUG_FLAG" ${SANITIZE:+SANITIZE="$SANITIZE"} $MAKE_EXTRA || true
        [[ "$BUILD_CLIENT" == "1" ]] && CXX="$COMPILER" make -C "$PROJECT_ROOT" client DEBUG="$DEBUG_FLAG" ${SANITIZE:+SANITIZE="$SANITIZE"} $MAKE_EXTRA || true
        [[ "$BUILD_TESTS" == "1" ]] && CXX="$COMPILER" make -C "$PROJECT_ROOT" test DEBUG="$DEBUG_FLAG" ${SANITIZE:+SANITIZE="$SANITIZE"} $MAKE_EXTRA || true
    fi
fi

echo ""
echo "Artifacts in $BIN_DIR:"
ls -lh "$BIN_DIR" || true

if [[ "$BUILD_TESTS" == "1" && -f "$BIN_DIR/audiorouter_tests" ]]; then
    if [[ "$CLEAN" == "0" ]]; then
        # Tests already ran via make test, but show summary
        :
    else
        log_info "Running tests..."
        "$BIN_DIR/audiorouter_tests"
    fi
fi

echo ""
echo "============================================================"
echo " Build complete"
echo "============================================================"
echo " Server: $BIN_DIR/audiorouter_server  (if built)"
echo " Client: $BIN_DIR/audiorouter_client  (if built)"
echo " Tests : $BIN_DIR/audiorouter_tests   (if built)"
echo ""
echo " Run:"
echo "   $BIN_DIR/audiorouter_server --list-if --help"
echo "   $BIN_DIR/audiorouter_client --list-devices --help"
echo "============================================================"
