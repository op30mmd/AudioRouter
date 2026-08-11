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
// AAudio is the NDK's high-performance audio API: the stream is owned by
// AudioFlinger / the audio HAL, so playback works through the normal Android
// audio policy (speaker, Bluetooth, USB, ...) WITHOUT ROOT and WITHOUT
// touching /dev/snd or ALSA. This is the only client backend that runs on
// stock, non-rooted devices. The cost is that it is a "managed" path: the
// audio is mixed by Android (a notification can duck it) and the stream can
// be preempted on routing changes (headphones unplugged, BT reconnect).
//
// Design (mirrors the standalone `stream_daemon` tool in src/tools/):
//   - PCM is written into a FIFO at /data/local/tmp/audiorouter_aaudio.fifo.
//   - A background pump thread reads the FIFO (blocking semantics, never
//     EOF because the fd is opened O_RDWR) and feeds AAudioStream_write()
//     in ~20 ms chunks with a 500 ms timeout, exactly like the standalone
//     daemon. The FIFO gives natural back-pressure: when AAudio consumes
//     slower than the network delivers, the pipe fills and write_frames()
//     blocks, which drains the jitter buffer instead of dropping.
//   - Underrun/pause recovery: if the stream is PAUSED/STOPPED or a write
//     fails, the stream is requestStart()ed again; if it is DISCONNECTED
//     (routing change), the stream is recreated from the builder config
//     (rate-limited).
//
// Device naming (select with -d):
//   aaudio              -> USAGE_MEDIA + LOW_LATENCY (default, like the daemon)
//   aaudio:deep         -> USAGE_MEDIA + PERFORMANCE_MODE_NONE (power saver /
//                          deep buffer; more stable, slightly higher latency)
//   aaudio:voip         -> USAGE_VOICE_COMMUNICATION + LOW_LATENCY (routes
//                          like a call; useful when the stock audio policy
//                          otherwise ducks the stream)
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

private:
#if defined(__ANDROID__) && __ANDROID_API__ >= 26
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
    // stream can't flood the log at 50 lines/second).
    uint64_t last_write_warn_ms_ = 0;
};

} // namespace audiorouter
