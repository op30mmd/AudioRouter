CXX ?= g++
# -MMD -MP generate per-object .d dependency files so header changes
# (e.g. agm_fifo_player.hpp) trigger recompilation of every consumer instead
# of leaving stale .o files with a mismatched class layout.
CXXFLAGS ?= -std=c++23 -O3 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -pthread -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -MMD -MP
INCLUDES = -Isrc/common -Isrc/client -Isrc/server
LDFLAGS_EXTRA = -pie
DEPFILES = $(patsubst %.o,%.d,$(COMMON_OBJS) $(SERVER_OBJS) $(CLIENT_OBJS) $(TEST_OBJS))

# Debug / Sanitizer variant: make DEBUG=1 or make SANITIZE=address,undefined
ifeq ($(DEBUG),1)
    CXXFLAGS := -std=c++23 -O0 -g -Wall -Wextra -Wpedantic -Wconversion -pthread -fno-omit-frame-pointer -fstack-protector-strong -MMD -MP
    ifeq ($(SANITIZE),)
        SANITIZE=address,undefined
    endif
endif

ifneq ($(SANITIZE),)
    CXXFLAGS += -fsanitize=$(SANITIZE) -fno-sanitize-recover=all
    LDFLAGS_EXTRA += -fsanitize=$(SANITIZE)
endif

# Platform detection
UNAME_S := $(shell uname -s)
ifeq ($(OS),Windows_NT)
    PLATFORM := windows
    SERVER_LIBS = -lws2_32 -liphlpapi -lavrt -lole32
    CLIENT_LIBS = -lws2_32 -liphlpapi
    EXE_EXT = .exe
    WINDRES ?= windres
    SERVER_RESOURCE_OBJ = $(BUILD_DIR)/server_manifest.o
else
    PLATFORM := linux
    SERVER_LIBS = -lpthread
    CLIENT_LIBS = -lpthread -ldl
    EXE_EXT =
    SERVER_RESOURCE_OBJ =
endif

# AAudio (Android API 26+) support: the aaudio player and the standalone
# stream_daemon are compiled in only when the toolchain targets Android and
# can link libaaudio (NDK sysroot, or Termux with the ndk-sysroot package).
# On plain Linux/CI hosts these probes fail and aaudio_player.cpp compiles as
# a no-op stub, so the client still builds everywhere.
ANDROID_TARGET := $(shell echo 'int main(){return 0;}' | $(CXX) -x c++ -dM -E - 2>/dev/null | grep -q '__ANDROID__' && echo 1)
ANDROID_TRIPLE := $(shell $(CXX) -dumpmachine 2>/dev/null)
ANDROID_TRIPLE_BASE := $(shell printf '%s' '$(ANDROID_TRIPLE)' | sed -E 's/[0-9]+$$//')
ifeq ($(ANDROID_TARGET),1)
    # Termux's bionic defaults to API 24; AAudio requires 26+.
    # Hoisting with -D__ANDROID_API__=26 alone is NOT enough: bionic gates
    # fortify declarations on __ANDROID_MIN_SDK_VERSION__ (android/versioning.h)
    # and clang enforces availability attributes (strtof_l, strtod_l,
    # __sendto_chk: introduced in 26) against the API level baked into the
    # target triple, neither of which a -D macro can change. Raise the triple.
    # Target API 30 to match the proven standalone stream_daemon build
    # (clang++ -target aarch64-linux-android30), which plays correctly on the
    # reference device. 26+ is required for AAudio.
    CXXFLAGS += --target=$(ANDROID_TRIPLE_BASE)30 -Wno-unavailable-declarations
endif
# Probe with the same raised target triple used for compilation (a -D define
# is not enough for library selection). Termux's sysroot ships no libaaudio
# stub, so fall back to the device's system lib (Android 8+ has
# /system/lib64/libaaudio.so), linked by absolute path - no -L search needed.
ANDROID_LIBDIR := $(if $(findstring aarch64,$(ANDROID_TRIPLE_BASE)),/system/lib64,/system/lib)
AAUDIO_SYSROOT_LINKABLE := $(shell echo 'int main(){return 0;}' | $(CXX) -x c++ --target=$(ANDROID_TRIPLE_BASE)30 -laaudio -o /dev/null 2>/dev/null && echo 1)
AAUDIO_SYSTEM_AVAILABLE := $(shell test -f $(ANDROID_LIBDIR)/libaaudio.so && echo 1)
ifeq ($(AAUDIO_SYSROOT_LINKABLE),1)
    CLIENT_LIBS += -laaudio
    HAVE_AAUDIO = 1
else ifeq ($(AAUDIO_SYSTEM_AVAILABLE),1)
    CLIENT_LIBS += $(ANDROID_LIBDIR)/libaaudio.so
    HAVE_AAUDIO = 1
endif
ifeq ($(HAVE_AAUDIO),1)
    # Compile the real AAudio backend only when libaaudio is linkable
    # (it is NOT on stock Termux); otherwise aaudio_player.cpp must stub out,
    # or the link fails with undefined AAudio* symbols.
    CXXFLAGS += -DAAUDIO_ENABLED=1
endif

# PulseAudio playback backend (-d pulse). Compiled in when pkg-config finds
# libpulse-simple (Debian/Ubuntu: libpulse-dev, Termux: pulseaudio). Without it
# pulse_player.cpp builds as a stub whose open() fails fast, so the client still
# builds everywhere. Force off with: make PULSEAUDIO=0
PULSEAUDIO ?= 1
ifeq ($(PULSEAUDIO),1)
    PULSE_CFLAGS := $(shell pkg-config --cflags libpulse-simple 2>/dev/null)
    PULSE_LIBS := $(shell pkg-config --libs libpulse-simple 2>/dev/null)
    ifneq ($(PULSE_LIBS),)
        # pkg-config reports the HOST library. When $(CXX) is a cross compiler
        # (aarch64/Android) that libpulse is the wrong architecture, so confirm
        # it actually compiles AND links with this very compiler before
        # enabling the backend - same approach as the libaaudio probe above.
        # '#' cannot appear literally inside $(shell ...) (make treats it as a
        # comment), so build it from its octal escape via printf.
        # Termux has no /tmp: honour $TMPDIR (falling back to the build dir,
        # which always exists here) or the probe would fail to write its
        # source file and silently disable the backend on-device.
        PULSE_PROBE_DIR := $(if $(TMPDIR),$(TMPDIR),.)
        PULSE_LINKABLE := $(shell printf '\043include <pulse/simple.h>\nint main(){ pa_simple_free(0); return 0; }\n' \
            > "$(PULSE_PROBE_DIR)/.ar_pulse_probe.cpp" 2>/dev/null && \
            $(CXX) "$(PULSE_PROBE_DIR)/.ar_pulse_probe.cpp" $(PULSE_CFLAGS) $(PULSE_LIBS) -o /dev/null 2>/dev/null \
            && echo 1; rm -f "$(PULSE_PROBE_DIR)/.ar_pulse_probe.cpp")
        ifeq ($(PULSE_LINKABLE),1)
            HAVE_PULSEAUDIO = 1
            CXXFLAGS += -DPULSEAUDIO_ENABLED=1 $(PULSE_CFLAGS)
            CLIENT_LIBS += $(PULSE_LIBS)
        endif
    endif
endif

BUILD_DIR = build
BIN_DIR = bin

COMMON_SRCS = src/common/socket_util.cpp
COMMON_OBJS = $(BUILD_DIR)/socket_util.o

SERVER_SRCS = src/server/main.cpp \
              src/server/server.cpp \
              src/server/wasapi_capture.cpp \
              src/server/dummy_capture.cpp \
              src/server/audio_endpoint_control.cpp
SERVER_OBJS = $(patsubst src/server/%.cpp,$(BUILD_DIR)/server_%.o,$(SERVER_SRCS))

CLIENT_SRCS = src/client/main.cpp \
              src/client/client.cpp \
              src/client/alsa_player.cpp \
              src/client/direct_alsa.cpp \
              src/client/agm_fifo_player.cpp \
              src/client/aaudio_player.cpp \
              src/client/termux_api_player.cpp \
              src/client/pulse_player.cpp \
              src/client/dummy_player.cpp \
              src/client/jitter_buffer.cpp \
              src/client/android_helpers.cpp
CLIENT_OBJS = $(patsubst src/client/%.cpp,$(BUILD_DIR)/client_%.o,$(CLIENT_SRCS))

STREAM_DAEMON_SRCS = src/tools/stream_daemon.cpp
STREAM_DAEMON_TARGET = $(BIN_DIR)/stream_daemon$(EXE_EXT)

TEST_SRCS = tests/test_main.cpp \
            tests/test_protocol.cpp \
            tests/test_ring_buffer.cpp \
            tests/test_jitter_buffer.cpp \
            tests/test_socket.cpp \
            tests/test_usb_tunnel.cpp \
            tests/test_conversion.cpp \
            tests/test_termux_api.cpp \
            tests/test_pulse.cpp \
            tests/test_thread_safety.cpp \
            tests/test_type_safety.cpp \
            tests/test_memory_safety.cpp
TEST_OBJS = $(patsubst tests/%.cpp,$(BUILD_DIR)/test_%.o,$(TEST_SRCS))

SERVER_TARGET = $(BIN_DIR)/audiorouter_server$(EXE_EXT)
CLIENT_TARGET = $(BIN_DIR)/audiorouter_client$(EXE_EXT)
TEST_TARGET = $(BIN_DIR)/audiorouter_tests$(EXE_EXT)

.PHONY: all clean test server client directories sanitize stream-daemon

all: directories $(SERVER_TARGET) $(CLIENT_TARGET) $(TEST_TARGET)
ifeq ($(HAVE_AAUDIO),1)
all: $(STREAM_DAEMON_TARGET)
endif

server: directories $(SERVER_TARGET)

client: directories $(CLIENT_TARGET)

ifeq ($(HAVE_AAUDIO),1)
stream-daemon: directories $(STREAM_DAEMON_TARGET)

$(STREAM_DAEMON_TARGET): $(STREAM_DAEMON_SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@ $(CLIENT_LIBS)
	@echo "Built: $(STREAM_DAEMON_TARGET)"
else
stream-daemon:
	@echo "stream_daemon needs an Android API 26+ toolchain that can link libaaudio."
	@echo "Build it with the NDK (or run ./scripts/build_stream_daemon.sh):"
	@echo "  clang++ -O2 -Wno-unavailable-declarations -target aarch64-linux-android30 -o stream_daemon src/tools/stream_daemon.cpp -laaudio -lm"
endif

test: directories $(TEST_TARGET)
	@echo "Running AudioRouter unit tests..."
	@$(TEST_TARGET)

sanitize:
	$(MAKE) clean
	$(MAKE) all DEBUG=1 SANITIZE=address,undefined
	@echo "Running with sanitizers..."
	@$(BIN_DIR)/audiorouter_tests

directories:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR)

# Common objects
$(BUILD_DIR)/socket_util.o: src/common/socket_util.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Server objects
$(BUILD_DIR)/server_%.o: src/server/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

ifeq ($(PLATFORM),windows)
# Embed the UAC manifest for MinGW/Make builds. Requiring elevation lets the
# loopback stream include audio sessions created by elevated applications.
$(SERVER_RESOURCE_OBJ): src/server/audiorouter_server.rc src/server/audiorouter_server.manifest
	$(WINDRES) --include-dir src/server $< -O coff -o $@
endif

# Client objects
$(BUILD_DIR)/client_%.o: src/client/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Test objects
$(BUILD_DIR)/test_%.o: tests/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Link Server
$(SERVER_TARGET): $(SERVER_OBJS) $(COMMON_OBJS) $(SERVER_RESOURCE_OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS_EXTRA) $^ -o $@ $(SERVER_LIBS)
	@echo "Built: $(SERVER_TARGET)"

# Link Client
$(CLIENT_TARGET): $(CLIENT_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS_EXTRA) $^ -o $@ $(CLIENT_LIBS)
	@echo "Built: $(CLIENT_TARGET)"

# Link Tests
$(TEST_TARGET): $(TEST_OBJS) $(BUILD_DIR)/client_jitter_buffer.o $(BUILD_DIR)/client_termux_api_player.o $(BUILD_DIR)/client_pulse_player.o $(BUILD_DIR)/client_android_helpers.o $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS_EXTRA) $^ -o $@ $(CLIENT_LIBS)
	@echo "Built: $(TEST_TARGET)"

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
	@echo "Cleaned build artifacts."

# Pull in auto-generated header dependencies (see -MMD -MP above).
-include $(DEPFILES)
