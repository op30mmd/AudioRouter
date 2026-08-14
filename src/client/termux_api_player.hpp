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
// riff_sz / data_sz of 0xFFFFFFFF mean "streaming size" (the player patches
// exact sizes into the file before handing it to MediaPlayer).
std::vector<uint8_t> build_wav_header(uint32_t sample_rate, uint16_t channels,
                                      bool float_format, uint32_t riff_sz,
                                      uint32_t data_sz);

// Gapless segment chaining gate. The open segment is handed to the media
// player once it holds `target` of audio AND the wall clock has advanced that
// far since the segment started (the wall gate stops a jitter-buffer prefill
// burst from skipping audio ahead of real time). The target is
//
//   max(S - L - lead, L + margin, floor)
//
// where S is the segment length, L the measured media-player command latency,
// and lead the handover lead. The S - L - lead term lands each play exactly
// as the outgoing segment ends; the L + margin term keeps the issue cadence
// slower than the command latency — the single issuer thread must finish one
// play before the next is due — which is what keeps playback CONTINUOUS when
// L is large (am broadcast to a cold/frozen app), at the cost of ~2L
// end-to-end delay. The floor keeps segments long enough for prepare().
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
// Termux:API app installed alongside Termux. When the client runs as root
// (e.g. via su for -b/--bind), the client pre-labels the segment pool and
// drops to the Termux app user after the socket binding, so this player
// always runs with sandbox access.
//
// MediaPlayer cannot play a pipe or a growing file (it sizes its source at
// prepare() time), so the stream is cut into short self-contained WAV
// segments. Each segment is written to a file from a small fixed POOL under
// the Termux home — files that the app can read (shared android:sharedUserId
// sandbox; the pool is pre-labeled with the app-data SELinux context while
// the client still has root) and that are recycled with O_TRUNC so the
// labels survive the privilege drop. Each finalized segment is handed over
// with `--es api_method MediaPlayer -a play`.
//
// Segments are chained GAPLESSLY (see termux_api::segment_ready): the play
// for segment N+1 is issued while segment N is still finishing, so the
// player's reset/start latency hides inside the handover lead of the
// outgoing segment. All media-player IPC runs on a dedicated ISSUER THREAD:
// the playback thread only records, paces, finalizes and hands off — a slow
// `am broadcast` (up to seconds on a frozen app) can therefore never stall
// the jitter-buffer pops. In steady state the end-to-end delay is ~S when
// the command latency is small (socket protocol), and ~2 x command latency
// when commands are slow (am broadcast), with continuous audio either way.
//
// Commands prefer the termux-api listen-socket protocol (mirrors the official
// termux-api binary; gives synchronous results) and otherwise go through
// `am broadcast` carrying the client's own result-socket extras — the app
// then writes its result (after prepare()+start()) to the client's socket,
// which is exactly how the official termux-api binary works on Android 14+
// where the app's own listen socket freezes. A lightweight watchdog resumes
// a paused player; every scheduled segment boundary re-issues `play`
// regardless, so the stream self-heals from a dead player at the next
// boundary. Apps too old to answer over the result sockets run in a blind
// fallback mode (delivery only, fixed latency estimate, logcat advised).
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
    // Prepares the segment cache for the Termux:API sandbox. As root it
    // creates the cache dir and the segment pool with Termux-app ownership
    // and the app-data SELinux context (restorecon), so files recycled after
    // the client's privilege drop stay readable by com.termux.api. The
    // client calls this right before dropping privileges; open() calls it
    // again when it still runs as root (no drop happened).
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
    uint32_t segment_ms_ = 2000;
    bool segment_explicit_ = false;       // termux:<ms> given by the user
    std::string cache_dir_;
    int seg_fd_ = -1;                     // open segment file (WAV being recorded)
    uint64_t seg_index_ = 0;
    std::string seg_path_;
    uint64_t seg_start_wall_ms_ = 0;      // wall time of the segment's first frame
    size_t seg_frames_ = 0;               // frames recorded into the open segment
    std::vector<uint8_t> convert_buf_;    // S24/S32 -> S16LE scratch
    uint64_t drops_ = 0;                  // segments replaced in the handoff slot
    uint64_t last_drop_warn_ms_ = 0;

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
    // blind am broadcast mode (old app without a result channel): no EMA
    // updates, no watchdog.
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
    void discard_segment_locked();
    // Patches exact sizes into the open segment and hands it to the issuer
    // thread; on a busy handoff slot the pending segment is replaced by this
    // fresher one. Caller holds io_mutex_.
    void finalize_segment_locked();
    void issuer_loop();
    void watchdog_loop();
};

} // namespace audiorouter
