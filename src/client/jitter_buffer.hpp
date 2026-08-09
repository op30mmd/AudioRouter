#pragma once

#include "../common/audio_types.hpp"
#include "../common/protocol.hpp"

#include <vector>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <atomic>
#include "../common/span_compat.hpp"
#include "../common/expected_compat.hpp"
#include <concepts>

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
    explicit JitterBuffer(uint32_t target_latency_ms = 35) noexcept;
    ~JitterBuffer() = default;

    // Non-copyable, movable
    JitterBuffer(const JitterBuffer&) = delete;
    JitterBuffer& operator=(const JitterBuffer&) = delete;
    JitterBuffer(JitterBuffer&&) noexcept;
    JitterBuffer& operator=(JitterBuffer&&) noexcept;

    void configure(const AudioConfig& config, uint32_t target_latency_ms);
    void reset() noexcept;

    // C++23 span-based API — preferred, bounds-checked
    [[nodiscard]] std::expected<bool, std::string> push_packet(uint32_t seq_num, uint64_t timestamp_us,
                                                               std::span<const int16_t> pcm) noexcept;
    // Legacy raw pointer API — hardened
    bool push_packet(uint32_t seq_num, uint64_t timestamp_us, const void* pcm_data, size_t num_frames) noexcept;

    // Pop frames — span version
    size_t pop_frames(std::span<int16_t> dest) noexcept;
    // Legacy
    size_t pop_frames(int16_t* dest, size_t num_frames) noexcept;

    [[nodiscard]] bool is_ready() const noexcept;
    [[nodiscard]] size_t available_frames() const noexcept;
    [[nodiscard]] double available_duration_ms() const noexcept;
    [[nodiscard]] JitterBufferStats get_stats() const noexcept;

    // Validation helper
    [[nodiscard]] bool is_configured() const noexcept { return config_.is_valid(); }

private:
    size_t available_frames_unlocked() const noexcept;
    double available_duration_ms_unlocked() const noexcept;
    void reset_unlocked() noexcept;
    bool push_packet_unlocked(uint32_t seq_num, uint64_t timestamp_us, std::span<const int16_t> pcm) noexcept;

    struct PacketSlot {
        uint32_t seq_num = 0;
        uint64_t timestamp_us = 0;
        std::vector<int16_t> pcm_data;
        size_t num_frames = 0;
        bool is_valid = false;
    };

    static constexpr size_t MAX_SLOTS = 256;

    AudioConfig config_{};
    uint32_t target_latency_ms_ = 35;
    size_t target_buffer_frames_ = 0;

    std::vector<PacketSlot> slots_;
    uint32_t next_play_seq_ = 0;
    bool has_first_packet_ = false;
    bool is_buffering_ = true;
    size_t slot_frame_offset_ = 0;

    JitterBufferStats stats_{};
    uint64_t last_arrival_timestamp_us_ = 0;
    double jitter_estimate_us_ = 0.0;
    int64_t last_transit_ = 0; // per-instance, not static
    mutable std::mutex mutex_;
};

} // namespace audiorouter
