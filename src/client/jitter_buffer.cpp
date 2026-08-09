#include "jitter_buffer.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <cmath>
#include <cstring>
#include <algorithm>

namespace audiorouter {

JitterBuffer::JitterBuffer(uint32_t target_latency_ms)
    : target_latency_ms_(target_latency_ms),
      target_buffer_frames_(0),
      slots_(MAX_SLOTS),
      next_play_seq_(0),
      has_first_packet_(false),
      is_buffering_(true),
      slot_frame_offset_(0),
      last_arrival_timestamp_us_(0),
      jitter_estimate_us_(0.0) {}

void JitterBuffer::configure(const AudioConfig& config, uint32_t target_latency_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    target_latency_ms_ = (target_latency_ms > 5) ? target_latency_ms : 5;
    target_buffer_frames_ = (static_cast<uint64_t>(config_.sample_rate) * target_latency_ms_) / 1000;
    reset();
}

void JitterBuffer::reset() {
    for (auto& slot : slots_) {
        slot.is_valid = false;
        slot.num_frames = 0;
        slot.pcm_data.clear();
    }
    next_play_seq_ = 0;
    has_first_packet_ = false;
    is_buffering_ = true;
    slot_frame_offset_ = 0;
    last_arrival_timestamp_us_ = 0;
    jitter_estimate_us_ = 0.0;
}

bool JitterBuffer::push_packet(uint32_t seq_num, uint64_t timestamp_us, const void* pcm_data, size_t num_frames) {
    if (!pcm_data || num_frames == 0) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t now_us = get_time_us();

    // Jitter calculation (RFC 3550 style exponential moving average)
    if (last_arrival_timestamp_us_ > 0 && timestamp_us > 0) {
        int64_t transit_diff = static_cast<int64_t>(now_us - timestamp_us);
        static int64_t last_transit = 0;
        if (last_transit != 0) {
            int64_t d = std::abs(transit_diff - last_transit);
            jitter_estimate_us_ += (static_cast<double>(d) - jitter_estimate_us_) / 16.0;
        }
        last_transit = transit_diff;
    }
    last_arrival_timestamp_us_ = now_us;

    stats_.packets_received++;

    if (!has_first_packet_) {
        has_first_packet_ = true;
        next_play_seq_ = seq_num;
        is_buffering_ = true;
    }

    // Sequence wrap-around distance check
    int32_t diff = static_cast<int32_t>(seq_num - next_play_seq_);

    if (diff < 0) {
        // Late packet arriving after slot has already played
        stats_.packets_out_of_order++;
        return false;
    }

    if (diff >= static_cast<int32_t>(MAX_SLOTS)) {
        // Too far ahead (buffer overrun or major gap) -> resync
        LOG_WARN("JitterBuffer: Massive sequence jump (diff=" << diff << "). Resyncing play pointer.");
        stats_.overruns++;
        next_play_seq_ = seq_num;
        diff = 0;
    }

    size_t slot_idx = seq_num % MAX_SLOTS;
    auto& slot = slots_[slot_idx];

    if (slot.is_valid && slot.seq_num == seq_num) {
        stats_.packets_duplicate++;
        return false; // Duplicate packet
    }

    // Store packet
    slot.seq_num = seq_num;
    slot.timestamp_us = timestamp_us;
    slot.num_frames = num_frames;

    size_t total_samples = num_frames * config_.channels;
    slot.pcm_data.resize(total_samples);
    std::memcpy(slot.pcm_data.data(), pcm_data, total_samples * sizeof(int16_t));
    slot.is_valid = true;

    // Check if we accumulated enough frames to exit buffering
    if (is_buffering_) {
        size_t buffered = 0;
        for (size_t i = 0; i < MAX_SLOTS; ++i) {
            size_t s = (next_play_seq_ + i) % MAX_SLOTS;
            if (slots_[s].is_valid && slots_[s].seq_num == (next_play_seq_ + i)) {
                buffered += slots_[s].num_frames;
            } else {
                break;
            }
        }

        if (buffered >= target_buffer_frames_) {
            is_buffering_ = false;
            LOG_DEBUG("JitterBuffer: Pre-buffering complete (" << buffered << " frames, "
                      << (buffered * 1000 / config_.sample_rate) << " ms). Starting ALSA playback.");
        }
    }

    return true;
}

size_t JitterBuffer::pop_frames(int16_t* dest, size_t num_frames) {
    if (!dest || num_frames == 0) return 0;

    std::lock_guard<std::mutex> lock(mutex_);

    if (is_buffering_ || !has_first_packet_) {
        // Fill output with silence while buffering
        std::fill(dest, dest + (num_frames * config_.channels), 0);
        return num_frames;
    }

    size_t frames_delivered = 0;

    while (frames_delivered < num_frames) {
        size_t slot_idx = next_play_seq_ % MAX_SLOTS;
        auto& slot = slots_[slot_idx];

        if (slot.is_valid && slot.seq_num == next_play_seq_) {
            // Slot is available! Extract frames
            size_t available_in_slot = slot.num_frames - slot_frame_offset_;
            size_t needed = num_frames - frames_delivered;
            size_t to_copy = std::min(available_in_slot, needed);

            const int16_t* src_ptr = slot.pcm_data.data() + (slot_frame_offset_ * config_.channels);
            int16_t* dst_ptr = dest + (frames_delivered * config_.channels);
            std::memcpy(dst_ptr, src_ptr, to_copy * config_.channels * sizeof(int16_t));

            frames_delivered += to_copy;
            slot_frame_offset_ += to_copy;

            if (slot_frame_offset_ >= slot.num_frames) {
                // Done with this slot
                slot.is_valid = false;
                slot_frame_offset_ = 0;
                next_play_seq_++;
            }
        } else {
            // Packet loss or gap! Conceal loss with silence / interpolation
            stats_.packets_lost++;
            stats_.underruns++;

            size_t needed = num_frames - frames_delivered;
            size_t conceal_frames = std::min(needed, static_cast<size_t>(config_.frames_per_packet > 0 ? config_.frames_per_packet : 240));

            int16_t* dst_ptr = dest + (frames_delivered * config_.channels);
            std::fill(dst_ptr, dst_ptr + (conceal_frames * config_.channels), 0);

            frames_delivered += conceal_frames;
            slot.is_valid = false;
            slot_frame_offset_ = 0;
            next_play_seq_++; // Skip the missing packet slot
        }
    }

    return frames_delivered;
}

bool JitterBuffer::is_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !is_buffering_ && has_first_packet_;
}

size_t JitterBuffer::available_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return available_frames_unlocked();
}

size_t JitterBuffer::available_frames_unlocked() const {
    size_t total = 0;
    for (size_t i = 0; i < MAX_SLOTS; ++i) {
        size_t s = (next_play_seq_ + i) % MAX_SLOTS;
        if (slots_[s].is_valid && slots_[s].seq_num == (next_play_seq_ + i)) {
            total += slots_[s].num_frames;
        } else {
            break;
        }
    }
    if (total >= slot_frame_offset_) {
        total -= slot_frame_offset_;
    }
    return total;
}

double JitterBuffer::available_duration_ms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return available_duration_ms_unlocked();
}

double JitterBuffer::available_duration_ms_unlocked() const {
    size_t frames = available_frames_unlocked();
    if (config_.sample_rate == 0) return 0.0;
    return (static_cast<double>(frames) * 1000.0) / static_cast<double>(config_.sample_rate);
}

JitterBufferStats JitterBuffer::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    JitterBufferStats s = stats_;
    s.current_buffer_ms = available_duration_ms_unlocked();
    s.avg_jitter_ms = jitter_estimate_us_ / 1000.0;
    return s;
}

} // namespace audiorouter
