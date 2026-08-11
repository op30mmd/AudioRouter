#pragma once

#include "../common/audio_types.hpp"
#include "../common/protocol.hpp"

#include <vector>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <atomic>

namespace audiorouter {

struct JitterBufferStats {
    uint64_t packets_received = 0;
    uint64_t packets_lost = 0;
    uint64_t packets_out_of_order = 0;
    uint64_t packets_duplicate = 0;
    uint64_t underruns = 0;
    uint64_t overruns = 0;
    double current_buffer_ms = 0.0;
    double avg_jitter_ms = 0.0;
};

class JitterBuffer {
public:
    explicit JitterBuffer(uint32_t target_latency_ms = 35);
    ~JitterBuffer() = default;

    void configure(const AudioConfig& config, uint32_t target_latency_ms);
    void reset();

    // Push an incoming audio packet into the jitter buffer
    bool push_packet(uint32_t seq_num, uint64_t timestamp_us, const void* pcm_data, size_t num_frames);

    // Read audio frames for ALSA playback. Returns number of frames provided.
    size_t pop_frames(int16_t* dest, size_t num_frames);

    bool is_ready() const;
    size_t available_frames() const;
    double available_duration_ms() const;
    JitterBufferStats get_stats() const;

private:
    size_t available_frames_unlocked() const;
    double available_duration_ms_unlocked() const;

    struct PacketSlot {
        uint32_t seq_num = 0;
        uint64_t timestamp_us = 0;
        std::vector<int16_t> pcm_data;
        size_t num_frames = 0;
        bool is_valid = false;
    };

    static constexpr size_t MAX_SLOTS = 256;

    AudioConfig config_;
    uint32_t target_latency_ms_;
    size_t target_buffer_frames_;
    // Larger prefill required for the first fill after a reset: packet delivery
    // is often slower than real-time during the first seconds of a stream
    // (network ramp-up, capture warm-up), so a shallow prefill runs dry.
    size_t startup_target_frames_;
    bool startup_pending_;
    // Stability gate: buffering exits only after this many consecutive,
    // gap-free packet arrivals, so playback never restarts into another
    // delivery stall (which caused repeated underruns during ramp-up).
    size_t hole_free_run_;
    uint32_t last_arrived_seq_;
    bool has_last_arrived_seq_;

    std::vector<PacketSlot> slots_;
    uint32_t next_play_seq_;
    bool has_first_packet_;
    bool is_buffering_;

    // Partial frame consumption within current slot
    size_t slot_frame_offset_;

    // Metrics & Skew tracking
    JitterBufferStats stats_;
    uint64_t last_arrival_timestamp_us_;
    int64_t last_transit_us_;
    double jitter_estimate_us_;
    mutable std::mutex mutex_;
};

} // namespace audiorouter
