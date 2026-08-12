// stream_daemon.cpp
//
// Standalone continuous real-time AAudio FIFO playback daemon for Android.
//
// Creates a named pipe (FIFO), opens it O_RDWR (so Linux never sends EOF when
// writers disconnect/pause), and streams whatever PCM is written into the pipe
// straight into an AAudio low-latency stream (48 kHz, stereo, S16). Any
// process can feed it:
//
//     # Terminal 1: run the daemon
//     ./stream_daemon
//     # Terminal 2: push raw interleaved S16 stereo PCM into the pipe
//     cat music.raw > /data/local/tmp/audio_pipe
//
// Usage:
//     stream_daemon [fifo_path] [sample_rate_hz]
//
//     fifo_path    default: /data/local/tmp/audio_pipe
//     sample_rate  default: 48000
//
// Build (Android API 26+; NDK sysroot or Termux with ndk-sysroot):
//     clang++ -O2 -Wno-unavailable-declarations -target aarch64-linux-android30 -o stream_daemon src/tools/stream_daemon.cpp -laaudio -lm
//     (or run: ./scripts/build_stream_daemon.sh)
//
// This is the reference implementation behind the client's '-d aaudio'
// backend (src/client/aaudio_player.cpp): the client integrates the same
// FIFO + AAudio streaming engine directly, so a separate daemon is only
// needed when another process owns the PCM.
//
// No root required - and root must NOT be used: AAudio goes through Android's
// audio HAL / AudioFlinger, and Android audio policy blocks the AAudio/MMAP
// data path for UID 0 (root has no app attribution token), so a root-launched
// stream opens but never renders. Run this daemon as the normal Termux user
// (u0_a...). The default FIFO path therefore lives under $HOME when set
// (the Termux home is writable by the app user; /data/local/tmp is not).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <cstdint>
#include <string>

#if defined(__ANDROID__) && __ANDROID_API__ >= 26
#include <aaudio/AAudio.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cerrno>

namespace {

constexpr int kChannels = 2;
constexpr int kBufferFrames = 960;  // ~20 ms buffer @ 48 kHz
constexpr int kBytesPerFrame = kChannels * static_cast<int>(sizeof(int16_t));

// Expand pipe capacity to 1 MB (~5 s audio buffer).
constexpr int kPipeSizeBytes = 1048576;
// AAudioStream_write() timeout: 500 ms.
constexpr long long kWriteTimeoutNs = 500000000LL;
// Minimum gap between stream recreations after a routing change.
constexpr long long kMinRebuildIntervalNs = 1000000000LL;

std::atomic<bool> g_stop{false};

void handle_signal(int) {
    g_stop.store(true);
}

long long monotonic_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

struct StreamHandle {
    AAudioStream* stream = nullptr;
    long long last_rebuild_ns = 0;
};

// Builds and starts a fresh AAudio stream (used on startup and after a
// disconnect).
bool open_stream(StreamHandle* h, int sample_rate) {
    if (h->stream != nullptr) {
        AAudioStream_close(h->stream);
        h->stream = nullptr;
    }

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t res = AAudio_createStreamBuilder(&builder);
    if (res != AAUDIO_OK || builder == nullptr) {
        std::printf("[-] Failed to create AAudio stream builder: %s\n",
                    AAudio_convertResultToText(res));
        return false;
    }
    AAudioStreamBuilder_setSampleRate(builder, sample_rate);
    AAudioStreamBuilder_setChannelCount(builder, kChannels);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);

    res = AAudioStreamBuilder_openStream(builder, &h->stream);
    AAudioStreamBuilder_delete(builder);
    if (res != AAUDIO_OK) {
        std::printf("[-] Failed to open AAudio stream: %s\n", AAudio_convertResultToText(res));
        h->stream = nullptr;
        return false;
    }
    AAudioStream_requestStart(h->stream);
    std::printf("[+] AAudio stream ready: %d Hz, %d ch, I16 (low latency)\n",
                AAudioStream_getSampleRate(h->stream), AAudioStream_getChannelCount(h->stream));
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Default FIFO: $HOME (writable by the non-root Termux user) when set,
    // else /data/local/tmp (the classic path, for root/shell environments).
    static std::string home_pipe;
    const char* fifo_path = "/data/local/tmp/audio_pipe";
    if (argc > 1) {
        fifo_path = argv[1];
    } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        home_pipe = std::string(home) + "/audio_pipe";
        fifo_path = home_pipe.c_str();
    }
    int sample_rate = 48000;
    if (argc > 2) {
        const int parsed = std::atoi(argv[2]);
        if (parsed > 0) sample_rate = parsed;
    }

    // AAudio is blocked for UID 0 by Android audio policy (root has no app
    // attribution token): the stream opens but never renders. Warn loudly so
    // a silent run is diagnosable.
    if (::getuid() == 0) {
        std::printf("[-] Warning: running as root (UID 0). Android audio policy blocks the\n");
        std::printf("    AAudio data path for root - the stream may open but never render.\n");
        std::printf("    Run this daemon as the normal Termux user (no su) for AAudio.\n");
    }

    // Re-create the pipe cleanly.
    ::unlink(fifo_path);
    if (::mkfifo(fifo_path, 0666) != 0 && errno != EEXIST) {
        std::printf("[-] mkfifo(%s) failed: %s\n", fifo_path, std::strerror(errno));
        return 1;
    }
    ::chmod(fifo_path, 0666);

    std::printf("=====================================================\n");
    std::printf("[+] Continuous Real-Time Audio Daemon Active\n");
    std::printf("[+] Pipe: %s\n", fifo_path);
    std::printf("[+] Rate: %d Hz, %d ch, S16\n", sample_rate, kChannels);
    std::printf("=====================================================\n");

    // CRITICAL: open O_RDWR so Linux NEVER sends EOF when writers
    // disconnect/pause.
    const int fd = ::open(fifo_path, O_RDWR);
    if (fd < 0) {
        std::printf("[-] Failed to open FIFO: %s\n", std::strerror(errno));
        return 1;
    }

    // Expand pipe capacity to 1 MB (~5 s audio buffer).
    ::fcntl(fd, F_SETPIPE_SZ, kPipeSizeBytes);

    StreamHandle handle;
    if (!open_stream(&handle, sample_rate)) {
        ::close(fd);
        return 1;
    }

    ::signal(SIGINT, handle_signal);
    ::signal(SIGTERM, handle_signal);

    uint8_t buffer[kBufferFrames * kBytesPerFrame];
    size_t residual = 0;
    long long zero_write_count = 0;

    std::printf("[+] Streaming engine active! Waiting for audio data...\n");
    std::printf("[+] Feed it:  cat audio.raw > %s\n", fifo_path);

    while (!g_stop.load()) {
        // Continuous blocking read (never returns 0 / EOF on an O_RDWR pipe).
        // poll() bounds the wait so Ctrl+C stays responsive.
        struct pollfd pfd = {fd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0 || !(pfd.revents & POLLIN)) continue;

        ssize_t bytesRead = ::read(fd, buffer + residual, sizeof(buffer) - residual);
        if (bytesRead <= 0) continue;

        const size_t totalBytes = static_cast<size_t>(bytesRead) + residual;
        const int32_t framesAvailable = static_cast<int32_t>(totalBytes / kBytesPerFrame);
        residual = totalBytes % kBytesPerFrame;

        if (framesAvailable > 0) {
            // A previous reconnect attempt may have failed; retry (rate-
            // limited) instead of touching a null stream.
            if (handle.stream == nullptr) {
                const long long now_ns = monotonic_ns();
                if (now_ns - handle.last_rebuild_ns >= kMinRebuildIntervalNs) {
                    handle.last_rebuild_ns = now_ns;
                    open_stream(&handle, sample_rate);
                }
                continue;
            }

            // Auto-restart the stream if it underran / was paused / stopped.
            const aaudio_stream_state_t state = AAudioStream_getState(handle.stream);
            if (state == AAUDIO_STREAM_STATE_DISCONNECTED) {
                // Routing changed (headphones unplugged, BT reconnect, ...);
                // the old stream is dead, recreate it (rate-limited).
                const long long now_ns = monotonic_ns();
                if (now_ns - handle.last_rebuild_ns >= kMinRebuildIntervalNs) {
                    handle.last_rebuild_ns = now_ns;
                    std::printf("[-] Stream disconnected; recreating AAudio stream...\n");
                    open_stream(&handle, sample_rate);
                }
                continue;
            }
            if (state == AAUDIO_STREAM_STATE_PAUSED || state == AAUDIO_STREAM_STATE_STOPPED) {
                AAudioStream_requestStart(handle.stream);
            }

            // handle.stream is guaranteed non-null here (a failed reconnect
            // above continues the loop instead).
            const aaudio_result_t framesWritten =
                AAudioStream_write(handle.stream, buffer, framesAvailable, kWriteTimeoutNs);
            if (framesWritten < 0) {
                AAudioStream_requestStart(handle.stream);
            } else if (framesWritten == 0) {
                // 0 frames = the stream is not rendering (dead MMAP path;
                // commonly the root-UID block). Report it, throttled.
                ++zero_write_count;
                if (zero_write_count == 1 || zero_write_count % 200 == 0) {
                    std::printf("[-] AAudioStream_write returned 0 frames (%lld times) - "
                                "the stream is not rendering. Running as root blocks AAudio; "
                                "run the daemon as a normal user.\n",
                                static_cast<long long>(zero_write_count));
                }
            } else {
                zero_write_count = 0;
            }
        }

        // Keep unused fractional frame bytes for the next iteration.
        if (residual > 0) {
            std::memmove(buffer, buffer + (framesAvailable * kBytesPerFrame), residual);
        }
    }

    if (handle.stream != nullptr) {
        AAudioStream_close(handle.stream);
    }
    ::close(fd);
    std::printf("[+] Daemon stopped cleanly.\n");
    return 0;
}

#else  // !(__ANDROID__ && __ANDROID_API__ >= 26)

int main() {
    std::printf("stream_daemon requires an Android API 26+ toolchain with libaaudio.\n");
    std::printf("Build it with the NDK, e.g.:\n");
    std::printf("  clang++ -O2 -Wno-unavailable-declarations -target aarch64-linux-android30 \\\n");
    std::printf("      -o stream_daemon src/tools/stream_daemon.cpp -laaudio -lm\n");
    return 1;
}

#endif
