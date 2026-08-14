#pragma once

#include "audio_player.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace audiorouter {

// Pure, unit-testable helpers for the Termux:API backend (no Android
// dependencies; compiled and exercised on host builds too).
namespace termux_api {

// Scheduling state of the open segment: wall time of its first frame, frames
// written so far, and the stream sample rate.
struct SegmentClock {
    uint64_t seg_start_wall_ms = 0;
    size_t frames_written = 0;
    uint32_t sample_rate = 48000;
};

// Builds a canonical 44-byte RIFF/WAVE header for interleaved PCM audio.
// riff_sz / data_sz of 0xFFFFFFFF mean "streaming size" (the player patches
// exact sizes into the file before handing it to MediaPlayer).
std::vector<uint8_t> build_wav_header(uint32_t sample_rate, uint16_t channels,
                                      bool float_format, uint32_t riff_sz,
                                      uint32_t data_sz);

// Gapless segment chaining gate: the open segment is handed to the media
// player once it holds (segment_ms - latency_est_ms - issue_lead_ms) of audio
// AND the wall clock has advanced that far since the segment started. The
// content gate protects against network underruns, the wall gate stops a
// jitter-buffer prefill burst from skipping audio ahead of real time.
bool segment_ready(const SegmentClock& clock, uint64_t now_ms, uint32_t segment_ms,
                   double latency_est_ms, uint32_t issue_lead_ms);

// The termux-api command extras: --es api_method "MediaPlayer" -a <action>
// plus the optional --es file / socket extras. Over the app's listen-socket
// protocol this string is the entire wire payload (the app parses the extras
// and rejects any leftover tokens, so there is deliberately no `am broadcast`
// prefix); the plain-am fallback prepends
// "<am> broadcast --user 0 -n com.termux.api/.TermuxApiReceiver".
std::string build_command_line(const std::string& action, const std::string& file,
                               const std::string& in_addr, const std::string& out_addr);

} // namespace termux_api

// Termux:API backend: plays the stream through Android's MediaPlayer via the
// Termux:API app (the `termux-media-player` API). Needs NO root — just the
// Termux:API app installed alongside Termux.
//
// MediaPlayer cannot play a pipe or a growing file (it sizes its source at
// prepare() time), so the stream is cut into short self-contained WAV
// segments. Each segment is written to a file under the Termux home — which
// com.termux.api can read because Termux:API shares Termux's sandbox
// (android:sharedUserId) — and handed over with
// `am broadcast ... --es api_method MediaPlayer -a play --es file <path>`.
//
// Segments are chained GAPLESSLY (see termux_api::segment_ready): the play
// for segment N+1 is issued while segment N is still finishing, so the
// player's reset/start latency hides inside the last `issue_lead_ms` of the
// outgoing segment. The steady-state end-to-end delay is ~segment_ms; the
// boundary glitch is bounded by the error of the measured startup latency,
// which is tracked with an EMA — over the app's listen-socket protocol the
// command result only arrives after prepare()+start(), so the measurement
// includes MediaPlayer's real startup (including cold app processes).
//
// Commands prefer the termux-api listen-socket protocol (mirrors the official
// termux-api binary; gives synchronous results) and fall back to plain
// `am broadcast` on older Termux:API versions. A watchdog polls the player
// status while the socket protocol is available: a paused player is resumed
// and a dead one is restarted at live audio. Every scheduled segment boundary
// re-issues `play` anyway, so the stream also self-heals without a watchdog.
//
// Device naming (select with -d):
//   termux            -> 2 s segments (default: ~2 s delay, no root)
//   termux-api        -> alias for termux
//   termux:<ms>       -> custom segment length (400..10000 ms; shorter =
//                        lower delay, longer = fewer switch glitches)
//
// On non-Android builds the player compiles as a stub whose open() fails
// fast, letting the client's strategy fallback pick another backend.
class TermuxApiPlayer : public IAudioPlayer {
public:
    TermuxApiPlayer();
    ~TermuxApiPlayer() override;

    bool open(const AudioConfig& config, const std::string& device_name = "termux") override;
    void close() override;
    bool is_open() const override;

    size_t write_frames(const void* pcm_data, size_t num_frames) override;
    size_t get_buffer_delay_frames() const override;
    void flush() override;
    std::string get_device_name() const override;

    // True on Android builds (the backend is pure userspace; the real
    // availability check is api_available()).
    static bool is_supported();
    // True when the Termux:API app answers a media_player info command.
    static bool api_available();

private:
    // --- segment / scheduling state (guarded by io_mutex_) ---
    AudioConfig config_;
    std::string device_name_ = "termux";  // display name (-d value)
    uint32_t segment_ms_ = 2000;
    std::string cache_dir_;

    int seg_fd_ = -1;                 // open segment file (WAV being recorded)
    uint64_t seg_index_ = 0;
    std::string seg_path_;
    uint64_t seg_start_wall_ms_ = 0;  // wall time of the segment's first frame
    size_t seg_frames_ = 0;           // frames recorded into the open segment

    // Bookkeeping of the segment currently handed to MediaPlayer.
    std::string issued_path_prev_;    // previous issued segment (unlinked next)
    uint64_t issue_wall_ms_ = 0;
    size_t issued_frames_ = 0;

    double latency_est_ms_ = 300.0;   // EMA of the measured play startup
    // True while the termux-api listen-socket protocol works (results come
    // back synchronously and the watchdog can read the player status).
    std::atomic<bool> socket_protocol_{false};

    // Watchdog: polls the media player status and recovers a paused or dead
    // player (Android only; it acts only while the socket protocol is
    // available and discovers it on its own if the app's listener came up
    // after open()).
    std::thread watchdog_thread_;
    std::atomic<bool> stop_watchdog_{false};
    std::atomic<bool> recovering_{false};
    uint64_t last_recover_ms_ = 0;

    // Format conversion scratch (S24/S32 input -> S16LE WAV data).
    std::vector<uint8_t> convert_buf_;

    std::atomic<bool> is_open_{false};
    mutable std::mutex io_mutex_;

    // --- internals ---
    bool resolve_cache_dir();
    void cleanup_stale_segments();
    bool ensure_segment_locked();
    bool open_next_segment_locked();
    void discard_segment_locked();
    // Patches exact sizes into the open segment and hands it to the media
    // player; on success starts the next segment, on failure discards the
    // unplayed audio and resumes at live audio. Caller holds io_mutex_.
    void issue_segment_locked();
    void watchdog_loop();
};

} // namespace audiorouter
