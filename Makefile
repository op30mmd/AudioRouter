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
else
    PLATFORM := linux
    SERVER_LIBS = -lpthread
    CLIENT_LIBS = -lpthread -ldl
    EXE_EXT =
endif

# AAudio (Android API 26+) support: the aaudio player and the standalone
# stream_daemon are compiled in only when the toolchain targets Android and
# can link libaaudio (NDK sysroot, or Termux with the ndk-sysroot package).
# On plain Linux/CI hosts these probes fail and aaudio_player.cpp compiles as
# a no-op stub, so the client still builds everywhere.
ANDROID_TARGET := $(shell echo 'int main(){return 0;}' | $(CXX) -x c++ -dM -E - 2>/dev/null | grep -q '__ANDROID__' && echo 1)
ifeq ($(ANDROID_TARGET),1)
    # Termux's bionic defaults to API 24; AAudio requires 26+.
    CXXFLAGS += -D__ANDROID_API__=26 -Wno-unavailable-declarations
endif
AAUDIO_LINKABLE := $(shell echo 'int main(){return 0;}' | $(CXX) -x c++ -D__ANDROID_API__=26 -laaudio -o /dev/null 2>/dev/null && echo 1)
ifeq ($(AAUDIO_LINKABLE),1)
    CLIENT_LIBS += -laaudio
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
            tests/test_conversion.cpp \
            tests/test_thread_safety.cpp \
            tests/test_type_safety.cpp \
            tests/test_memory_safety.cpp
TEST_OBJS = $(patsubst tests/%.cpp,$(BUILD_DIR)/test_%.o,$(TEST_SRCS))

SERVER_TARGET = $(BIN_DIR)/audiorouter_server$(EXE_EXT)
CLIENT_TARGET = $(BIN_DIR)/audiorouter_client$(EXE_EXT)
TEST_TARGET = $(BIN_DIR)/audiorouter_tests$(EXE_EXT)

.PHONY: all clean test server client directories sanitize stream-daemon

all: directories $(SERVER_TARGET) $(CLIENT_TARGET) $(TEST_TARGET)
ifeq ($(AAUDIO_LINKABLE),1)
all: $(STREAM_DAEMON_TARGET)
endif

server: directories $(SERVER_TARGET)

client: directories $(CLIENT_TARGET)

ifeq ($(AAUDIO_LINKABLE),1)
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

# Client objects
$(BUILD_DIR)/client_%.o: src/client/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Test objects
$(BUILD_DIR)/test_%.o: tests/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Link Server
$(SERVER_TARGET): $(SERVER_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS_EXTRA) $^ -o $@ $(SERVER_LIBS)
	@echo "Built: $(SERVER_TARGET)"

# Link Client
$(CLIENT_TARGET): $(CLIENT_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS_EXTRA) $^ -o $@ $(CLIENT_LIBS)
	@echo "Built: $(CLIENT_TARGET)"

# Link Tests
$(TEST_TARGET): $(TEST_OBJS) $(BUILD_DIR)/client_jitter_buffer.o $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS_EXTRA) $^ -o $@ $(CLIENT_LIBS)
	@echo "Built: $(TEST_TARGET)"

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
	@echo "Cleaned build artifacts."

# Pull in auto-generated header dependencies (see -MMD -MP above).
-include $(DEPFILES)
