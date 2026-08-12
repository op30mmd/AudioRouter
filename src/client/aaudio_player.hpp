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
// stock, non-rooted devices.
//
// IMPORTANT: AAudio must run as a NORMAL app user (u0_a...), never as root.
// Android audio policy blocks AAudio/MMAP data paths created by UID 0
// because root processes have no app attribution token: the stream opens and
// reaches STARTED but the data path never renders (every write returns 0).
// termux_run.sh detects '-d aaudio' and runs the client without su; this
// player also fails fast when it detects UID 0 so the client falls back to
// the root-capable backends (AGM/ALSA) instead of playing silence.
//
// Design (mirrors the standalone `stream_daemon` tool in src/tools/):
//   - PCM is written into a FIFO under $HOME (the Termux user's home, which
//     a non-root app can write - /data/local/tmp is shell-only), falling back
//     to /data/local/tmp when $HOME is unset.
//   - A background pump thread reads the FIFO (blocking semantics, never
//     EOF because the fd is opened O_RDWR) and feeds AAudioStream_write()
//     in ~20 ms chunks with a 200 ms timeout. The FIFO gives natural
//     back-pressure: when AAudio consumes slower than the network delivers,
//     the pipe fills and write_frames() blocks, which drains the jitter
//     buffer instead of dropping.
//   - Underrun/pause recovery: if the stream is PAUSED/STOPPED or a write
//     fails, the stream is requestStart()ed again; if it is DISCONNECTED
//     (routing change), the stream is recreated from the builder config
//     (rate-limited). A stream that reaches STARTED but whose data path
//     never produces timestamps (dead MMAP path) is detected at open() via
//     AAudioStream_getTimestamp and retried with the deep-buffer
//     performance mode (legacy AudioTrack path), then given up on.
//
// Device naming (select with -d):
//   aaudio              -> USAGE_MEDIA + LOW_LATENCY (default)
//   aaudio:deep         -> USAGE_MEDIA + PERFORMANCE_MODE_NONE (power saver /
//                          deep buffer; also the legacy-AudioTrack fallback)
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

    // Resolved FIFO path: $HOME/audiorouter_aaudio.fifo when HOME is set
    // (non-root Termux), else /data/local/tmp/audiorouter_aaudio.fifo.
    static std::string fifo_path();

private:
#if defined(__ANDROID__) && defined(AAUDIO_ENABLED)
    // In-process path (non-root, or root with AUDIOROUTER_AAUDIO_AS_ROOT):
    // creates the FIFO, opens the stream in this process, starts the pump
    // thread.
    bool open_in_process();
    // Root path: the AAudio stream is created in a forked helper process that
    // drops to the Termux app user (Android blocks the AAudio data path for
    // UID 0). The parent keeps root for the socket binding (-b auto) and for
    // the AGM/ALSA fallback, and only streams PCM into the FIFO.
    bool open_via_helper();
    void helper_child_main(int status_fd);
    void monitor_loop();
    void mark_dead();
    // Opens the AAudio stream, requests start, waits for STARTED and verifies
    // the device actually consumes audio (writes a probe chunk and watches
    // framesRead/timestamps). Retries once with the deep-buffer performance
    // mode. quiet: no LOG (forked child - the logger mutex may be held by a
    // vanished thread, so the child uses fprintf instead).
    bool open_stream_and_probe(bool quiet);
    void pump_loop();
    void configure_builder(void* builder);   // AAudioStreamBuilder*
    // Blocks (up to timeout_ms) until the stream is fully STARTED. Caller
    // must hold stream_mutex_. Returns false on timeout or if the stream
    // went DISCONNECTED/CLOSED/UNINITIALIZED.
    bool wait_for_start_locked(int timeout_ms);
    // Verifies the stream's data path is actually running: writes a small
    // silence chunk (retrying over a window - the clock model ramps after
    // STARTED) and checks that the device consumes it (framesRead advances or
    // a valid timestamp appears). On failure fills fail_reason with the
    // diagnostic detail (state, write results, frame counters). Caller must
    // hold stream_mutex_.
    bool probe_stream_ready(std::string* fail_reason);
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
    // Consecutive AAudioStream_write calls that failed or wrote only part of
    // the chunk (reset on a full write). Used to detect a stream that opened
    // but never actually renders (a session the HAL cannot start).
    int consecutive_write_failures_ = 0;
    // Number of times the pump rebuilt the stream due to persistent stalls.
    // After the first stall-rebuild, the next one falls back to the
    // deep-buffer performance mode, which starts on HALs that reject
    // low-latency sessions.
    int stall_rebuilds_ = 0;
    // Force PERFORMANCE_MODE_NONE when (re)building the stream: set by open()
    // when the low-latency stream never starts, and by the pump after
    // repeated mid-stream stalls.
    bool deep_retry_ = false;

    // Forked-helper state (root mode only): the helper process pid, the read
    // end of the status pipe ('R' ready, 'F' failed to open, 'D' died), and
    // the monitor thread that watches it.
    pid_t child_pid_ = -1;
    int status_fd_ = -1;
    std::thread monitor_thread_;
    std::atomic<bool> stop_monitor_{false};
    // FIFO path resolved for the CURRENT process (see fifo_path()). Stored so
    // create_fifo()/unlink use the same path the child inherited, even if the
    // child sets HOME to the Termux home.
    std::string resolved_fifo_;
};

} // namespace audiorouter
