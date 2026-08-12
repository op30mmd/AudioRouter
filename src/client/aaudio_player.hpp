#pragma once

#include "audio_player.hpp"
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>

namespace audiorouter {

// AAudio FIFO player: plays PCM through Android's native AAudio API
// (NDK <aaudio/AAudio.h>) by streaming it through a named pipe (FIFO).
//
// This is the client-side twin of the standalone `stream_daemon` tool
// (src/tools/stream_daemon.cpp) and deliberately mirrors its proven,
// battle-tested engine:
//   - the stream is opened in THIS process (no uid games, no fork helper -
//     the daemon runs as root via `sudo` and works, so root is fine here),
//   - no usage/content-type hints are set (the daemon sets none; they can
//     steer the HAL onto a path that never starts),
//   - there is NO readiness probe that fails open(): the daemon never fails,
//     it just keeps writing. A stream that takes a while to start (clock
//     model ramp-up, slow DSP bring-up) is given all the time it needs,
//   - AAudioStream_write() uses the daemon's 500 ms timeout,
//   - the pump only restarts the stream when it is PAUSED/STOPPED and
//     recreates it when it is DISCONNECTED (routing change).
//
// The one addition over the daemon is a very lenient watchdog: if the
// stream never consumes anything for a long time (~10 s of failed writes),
// the stream is recreated once (a wedged HAL session can clear on reopen),
// then the player gives up and the client falls back to the AGM/ALSA
// backends - instead of looping forever like the daemon would.
//
// Device naming (select with -d):
//   aaudio              -> PERFORMANCE_MODE_LOW_LATENCY (default)
//   aaudio:deep         -> PERFORMANCE_MODE_NONE (deep buffer / power saver)
//   aaudio:voip         -> accepted for compatibility (usage/content-type
//                          hints are intentionally NOT set - see above)
//
// Requires Android 8.0+ (API 26). On other platforms (host builds, older
// targets) the player compiles as a stub whose open() fails fast, letting the
// client's strategy fallback pick ALSA / direct PCM instead.
class AaudioFifoPlayer : public IAudioPlayer {
public:
    AaudioFifoPlayer();
    ~AaudioFifoPlayer() override;

    bool open(const AudioConfig& config, const std::string& device_name = "aaudio") override;
    void close() override;
    bool is_open() const override;

    size_t write_frames(const void* pcm_data, size_t num_frames) override;
    size_t get_buffer_delay_frames() const override;
    void flush() override;
    std::string get_device_name() const override;

    // True when this binary was compiled with AAudio support (Android API 26+
    // toolchain with libaaudio available).
    static bool is_supported();

    // Resolved FIFO path: /data/local/tmp/audiorouter_aaudio.fifo when
    // running as root (the stream_daemon path), else $HOME/audiorouter_aaudio.fifo
    // (the Termux app user's writable home), else the Termux home, else
    // /data/local/tmp.
    static std::string fifo_path();

private:
#if defined(__ANDROID__) && defined(AAUDIO_ENABLED)
    void pump_loop();
    void configure_builder(void* builder);   // AAudioStreamBuilder*
    bool ensure_stream_started_locked();
    bool rebuild_stream_locked();
    bool create_fifo();
    void destroy_fifo();
#endif

    AudioConfig config_;
    // Selected mode: "lowlatency" (default), "deep" or "voip".
    std::string mode_ = "lowlatency";
    std::string device_name_ = "aaudio";
    // FIFO fd (O_RDWR | O_NONBLOCK so the pipe never EOFs and shutdown can
    // unblock cleanly). Guarded by io_mutex_ for open()/close() transitions.
    int fifo_fd_ = -1;
    // AAudioStream* — opaque here so the header stays compilable on non-Android
    // targets; only the Android implementation (aaudio_player.cpp) casts it.
    void* stream_ = nullptr;
    // Serializes stream_ access (pump thread vs close()/rebuild).
    // mutable: get_buffer_delay_frames() snapshots live counters (const method).
    mutable std::mutex stream_mutex_;
    std::thread pump_thread_;
    std::atomic<bool> stop_pump_{false};
    std::atomic<bool> is_open_{false};
    // Frames inside the AAudio stream (written - read); updated by the pump.
    std::atomic<int64_t> frames_in_flight_{0};
    // Timestamp of the last stream recreation (rate limit for rebuilds).
    uint64_t last_rebuild_ms_ = 0;
    // Timestamp of the last write-failure warning (rate limit so a wedged
    // stream can't flood the log).
    uint64_t last_write_warn_ms_ = 0;
    // Consecutive AAudioStream_write calls that failed or wrote only part of
    // the chunk (reset on a full write). The watchdog uses this to detect a
    // stream that never renders.
    int consecutive_write_failures_ = 0;
    // Number of times the pump rebuilt the stream due to persistent stalls.
    int stall_rebuilds_ = 0;
    // Force PERFORMANCE_MODE_NONE when (re)building the stream: used by the
    // watchdog's retry (a deep-buffer session can start on HALs where the
    // low-latency session never renders).
    bool deep_retry_ = false;
    // FIFO path resolved for THIS process (see fifo_path()). Stored so
    // create_fifo()/unlink use the same path consistently.
    std::string resolved_fifo_;
};

} // namespace audiorouter
