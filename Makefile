CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -pthread
INCLUDES = -Isrc/common -Isrc/client -Isrc/server

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
              src/client/dummy_player.cpp \
              src/client/jitter_buffer.cpp \
              src/client/android_helpers.cpp
CLIENT_OBJS = $(patsubst src/client/%.cpp,$(BUILD_DIR)/client_%.o,$(CLIENT_SRCS))

TEST_SRCS = tests/test_main.cpp \
            tests/test_protocol.cpp \
            tests/test_ring_buffer.cpp \
            tests/test_jitter_buffer.cpp \
            tests/test_socket.cpp \
            tests/test_conversion.cpp
TEST_OBJS = $(patsubst tests/%.cpp,$(BUILD_DIR)/test_%.o,$(TEST_SRCS))

SERVER_TARGET = $(BIN_DIR)/audiorouter_server$(EXE_EXT)
CLIENT_TARGET = $(BIN_DIR)/audiorouter_client$(EXE_EXT)
TEST_TARGET = $(BIN_DIR)/audiorouter_tests$(EXE_EXT)

.PHONY: all clean test server client directories

all: directories $(SERVER_TARGET) $(CLIENT_TARGET) $(TEST_TARGET)

server: directories $(SERVER_TARGET)

client: directories $(CLIENT_TARGET)

test: directories $(TEST_TARGET)
	@echo "Running AudioRouter unit tests..."
	@$(TEST_TARGET)

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
	$(CXX) $(CXXFLAGS) $^ -o $@ $(SERVER_LIBS)
	@echo "Built: $(SERVER_TARGET)"

# Link Client
$(CLIENT_TARGET): $(CLIENT_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(CLIENT_LIBS)
	@echo "Built: $(CLIENT_TARGET)"

# Link Tests
$(TEST_TARGET): $(TEST_OBJS) $(BUILD_DIR)/client_jitter_buffer.o $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(CLIENT_LIBS)
	@echo "Built: $(TEST_TARGET)"

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
	@echo "Cleaned build artifacts."
