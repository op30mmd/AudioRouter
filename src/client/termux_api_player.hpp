#pragma once

#include "audio_player.hpp"

#include <atomic>
#include <condition_variable>
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
// The player patches exact sizes into the header at segment creation (the
// file is pre-sized to its full length; see the class comment).
std::vector<uint8_t> build_wav_header(uint32_t sample_rate, uint16_t channels,
                                      bool float_format, uint32_t riff_sz,
                                      uint32_t data_sz);

// The issue gate of the file ring buffer. The open segment is handed to the
// media player once it holds `prefill_ms` of real audio AND the wall clock
// has advanced that far since the segment started (the wall gate stops a
// jitter-buffer prefill burst from skipping audio ahead of real time). The
// rest of the segment is already on disk (sparse zeros), so the player -
// which starts ~command-latency after the issue - can never read past the
// recording head: the end-to-end delay is prefill + command latency,
// independent of the segment length.
bool segment_ready(const SegmentClock& clock, uint64_t now_ms, uint32_t prefill_ms);

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
// Termux:API app installed alongside Termux. When the client runs as root
// (e.g. via su for -b/--bind), the client pre-labels the segment pool and
// drops to the Termux app user after the socket binding, so this player
// always runs with sandbox access.
//
// MediaPlayer cannot play a pipe or a growing file (it sizes its source at
// prepare() time), so the stream is laid out as a FILE RING BUFFER: each
// segment file is created at its full length - the header carries the exact
// sizes and the data region is a sparse hole that reads back as silence -
// and the stream is overwritten into it sequentially as it arrives. The
// player is handed a segment once the first `prefill` (~400 ms) of real
// audio is recorded; it starts ~command-latency later and plays the file at
// 1x while the recorder keeps overwriting the region just ahead. The
// end-to-end delay is therefore `prefill + command latency` — independent of
// the segment length, which only sets how often the player switches files
// (each switch stops the outgoing segment and prepares the next one, a
// ~prepare-time glitch per boundary).
//
// All media-player IPC runs on a dedicated ISSUER THREAD: the playback
// thread only records, paces, finalizes and hands off — a slow `am
// broadcast` (up to seconds on a frozen app) can therefore never stall the
// jitter-buffer pops. Commands prefer the termux-api listen-socket protocol
// and otherwise go through `am broadcast` carrying the client's own
// result-socket extras (the official termux-api mechanism on Android 14+,
// where the app's listen socket freezes); when the client runs in a
// root-shell SELinux domain its result sockets are labeled with the app's
// own context (captured as root before the privilege drop). Every play is
// confirmed by the app's own reply; a watchdog resumes a paused player and
// restarts a dead one at live audio. Only when the app cannot answer over
// the result sockets does the player run in a blind fallback mode.
//
// Device naming (select with -d):
//   termux            -> 10-minute files (one ~prepare-time switch pause
//                        every 10 minutes; the delay is ~prefill + command
//                        latency, NOT the file length; no root)
//   termux-api        -> alias for termux
//   termux:<ms>       -> custom file length (2000..3600000 ms; longer =
//                        fewer switch pauses, same delay, more disk)
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
    // Prepares the segment cache for the Termux:API sandbox. As root it
    // creates the cache dir and the segment pool with Termux-app ownership
    // and the app-data SELinux context (restorecon), and captures the app's
    // SELinux context so the dropped client can label its result sockets
    // with it. The client calls this right before dropping privileges;
    // open() calls it again when it still runs as root (no drop happened).
    static void prepare_cache_as_root();

private:
    // A finalized segment waiting for the issuer thread's play command.
    struct PendingSegment {
        std::string path;
        size_t frames = 0;
        uint64_t seg_index = 0;
        bool valid = false;
    };

    // --- recording state (playback thread; guarded by io_mutex_) ---
    AudioConfig config_;
    std::string device_name_ = "termux";  // display name (-d value)
    uint32_t segment_ms_ = 6000;          // file length (NOT the delay)
    std::string cache_dir_;
    int seg_fd_ = -1;                     // open segment file (WAV being recorded)
    uint64_t seg_index_ = 0;
    std::string seg_path_;
    uint64_t seg_start_wall_ms_ = 0;      // wall time of the segment's first frame
    size_t seg_frames_ = 0;               // frames recorded into the open segment
    size_t seg_total_frames_ = 0;         // full segment length (file ring size)
    bool seg_issued_ = false;             // this file's play was handed off
    std::vector<uint8_t> convert_buf_;    // S24/S32 -> S16LE scratch
    uint64_t drops_ = 0;                  // segments replaced in the handoff slot
    uint64_t last_drop_warn_ms_ = 0;
    uint64_t last_recover_ms_ = 0;        // watchdog recovery rate limit

    // --- handoff to the issuer thread (lock order: io_mutex_ -> handoff) ---
    // mutable: get_buffer_delay_frames() (const) reads the pending slot.
    mutable std::mutex handoff_mutex_;
    mutable std::condition_variable handoff_cv_;
    PendingSegment pending_;

    // --- issued-segment bookkeeping (issuer thread writes; stats read) ---
    std::atomic<uint64_t> issue_wall_ms_{0};
    std::atomic<size_t> issued_frames_{0};
    std::atomic<double> latency_est_ms_{300.0};  // EMA of the play command latency
    // True while the app answers commands with result text (listen-socket
    // protocol, or am broadcast with result sockets on Android 14+). False =
    // blind am broadcast mode (no result channel): the latency EMA then
    // tracks the am time plus a fixed prepare bias.
    std::atomic<bool> result_channel_{true};
    bool socket_path_ = false;                   // listen-socket protocol (vs am)
    int consecutive_no_result_ = 0;              // issuer thread only

    std::thread issuer_thread_;
    std::atomic<bool> stop_issuer_{false};
    std::thread watchdog_thread_;
    std::atomic<bool> stop_watchdog_{false};

    std::atomic<bool> is_open_{false};
    mutable std::mutex io_mutex_;

    // --- internals ---
    bool resolve_cache_dir();
    void cleanup_stale_segments();
    bool ensure_pool();
    bool open_next_segment_locked();
    // Restarts the recording at live audio: the open file is closed and the
    // next write opens a fresh pool file (a file already handed to the media
    // player is never truncated - the app may still be reading it; its slot
    // is only reused when the pool comes around again).
    void discard_segment_locked();
    // Hands the open segment to the issuer thread (once, at its prefill).
    // Recording CONTINUES in the same file until it is full - the file's
    // window is what the player will hear; a busy handoff slot replaces the
    // pending segment with this fresher one. Caller holds io_mutex_.
    void finalize_segment_locked();
    void issuer_loop();
    void watchdog_loop();
};

} // namespace audiorouter
