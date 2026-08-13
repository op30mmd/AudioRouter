#include "jitter_buffer.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <cmath>
#include <cstring>
#include <algorithm>

namespace {

// First-fill (post-reset) prefill is multiplied from the configured target and
// clamped to [kStartupPrefillMinMs, kStartupPrefillMaxMs]. The floor covers the
// typical 100-500ms server capture warm-up gap at stream start.
constexpr uint32_t kStartupPrefillMultiplier = 3;
constexpr uint32_t kStartupPrefillMinMs = 120;
constexpr uint32_t kStartupPrefillMaxMs = 500;

// Stability gate: buffering exits only after this many consecutive gap-free
// arrivals (~120ms at the 240-frame/5ms packet rate), so the playhead never
// restarts into a delivery stall that would immediately dry it out again.
constexpr uint32_t kStableArrivalCount = 24;

// Drain-to-target: after a burst (startup prefill, a stall that let the
// buffer accumulate, a reconnect) the playhead can sit far above the
// configured target latency. pop_frames() gently sheds the excess instead of
// leaving it as permanent added latency. Only drains when the buffered level
// exceeds target + margin; sheds at most one small chunk per interval, so the
// catch-up is a few near-inaudible 5ms skips rather than one big gap.
constexpr uint32_t kDrainMarginMs = 25;
constexpr uint32_t kDrainIntervalMs = 400;
constexpr size_t kDrainChunkFrames = 240;  // 5 ms @ 48 kHz
// Fast catch-up for large backlogs (see pop_frames): 10 ms skips every
// 100 ms shed ~100 ms/s so a post-stall backlog is gone in a couple of
// seconds instead of lingering as a constant audible lag.
constexpr uint32_t kDrainFastIntervalMs = 100;
constexpr size_t kDrainFastChunkFrames = 480;  // 10 ms @ 48 kHz
// Draining is disabled until this long after the first fill completes: the
// startup prefill is deliberate protection for the first seconds of delivery
// and must not be shed the moment playback starts.
constexpr uint32_t kDrainStartGraceMs = 3000;

} // namespace

namespace audiorouter {

JitterBuffer::JitterBuffer(uint32_t target_latency_ms)
    : target_latency_ms_(target_latency_ms),
      target_buffer_frames_(0),
      startup_target_frames_(0),
      startup_pending_(false),
      hole_free_run_(0),
      last_arrived_seq_(0),
      has_last_arrived_seq_(false),
      slots_(MAX_SLOTS),
      next_play_seq_(0),
      has_first_packet_(false),
      is_buffering_(true),
      slot_frame_offset_(0),
      last_arrival_timestamp_us_(0),
      last_transit_us_(0),
      jitter_estimate_us_(0.0) {}

void JitterBuffer::configure(const AudioConfig& config, uint32_t target_latency_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    target_latency_ms_ = (target_latency_ms > 5) ? target_latency_ms : 5;
    target_buffer_frames_ = (static_cast<uint64_t>(config_.sample_rate) * target_latency_ms_) / 1000;

    uint32_t startup_ms = std::max(kStartupPrefillMinMs, target_latency_ms_ * kStartupPrefillMultiplier);
    startup_ms = std::min(startup_ms, kStartupPrefillMaxMs);
    startup_target_frames_ = (static_cast<uint64_t>(config_.sample_rate) * startup_ms) / 1000;

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
    startup_pending_ = true;
    hole_free_run_ = 0;
    has_last_arrived_seq_ = false;
    last_arrived_seq_ = 0;
    slot_frame_offset_ = 0;
    last_arrival_timestamp_us_ = 0;
    last_drain_ms_ = 0;
    startup_complete_ms_ = 0;
    last_transit_us_ = 0;
    jitter_estimate_us_ = 0.0;
}

bool JitterBuffer::push_packet(uint32_t seq_num, uint64_t timestamp_us, const void* pcm_data, size_t num_frames) {
    if (!pcm_data || num_frames == 0) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t now_us = get_time_us();

    // Jitter calculation (RFC 3550 style exponential moving average)
    if (last_arrival_timestamp_us_ > 0 && timestamp_us > 0) {
        int64_t transit_diff = static_cast<int64_t>(now_us - timestamp_us);
        if (last_transit_us_ != 0) {
            int64_t d = std::abs(transit_diff - last_transit_us_);
            jitter_estimate_us_ += (static_cast<double>(d) - jitter_estimate_us_) / 16.0;
        }
        last_transit_us_ = transit_diff;
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
        if (diff > -static_cast<int32_t>(MAX_SLOTS)) {
            // Late packet arriving after slot has already played
            stats_.packets_out_of_order++;
            return false;
        } else {
            // Massive backward sequence jump (server restart or client sequence desync)
            LOG_WARN("JitterBuffer: Massive backward sequence jump (diff=" << diff << "). Resyncing play pointer.");
            for (auto& s : slots_) {
                s.is_valid = false;
                s.pcm_data.clear();
            }
            next_play_seq_ = seq_num;
            is_buffering_ = true;
            slot_frame_offset_ = 0;
            diff = 0;
        }
    }

    if (diff >= static_cast<int32_t>(MAX_SLOTS)) {
        // Too far ahead (buffer overrun or major gap) -> resync
        LOG_WARN("JitterBuffer: Massive sequence jump (diff=" << diff << "). Resyncing play pointer.");
        stats_.overruns++;
        for (auto& s : slots_) {
            s.is_valid = false;
            s.pcm_data.clear();
        }
        next_play_seq_ = seq_num;
        is_buffering_ = true;
        slot_frame_offset_ = 0;
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

    // Track the run of consecutive (gap-free) arrivals for the stability gate.
    if (has_last_arrived_seq_ && seq_num == last_arrived_seq_ + 1) {
        hole_free_run_++;
    } else {
        hole_free_run_ = 1;
    }
    last_arrived_seq_ = seq_num;
    has_last_arrived_seq_ = true;

    // Check if we accumulated enough frames to exit buffering. The prefill is
    // the (larger) startup target for the first fill after a reset and the
    // configured target for later re-buffers; the stability gate additionally
    // requires the recent delivery tail to be gap-free so the playhead doesn't
    // restart into a stall and dry out again.
    if (is_buffering_) {
        const size_t fill_target = startup_pending_ ? startup_target_frames_ : target_buffer_frames_;

        size_t buffered = 0;
        for (size_t i = 0; i < MAX_SLOTS; ++i) {
            size_t s = (next_play_seq_ + i) % MAX_SLOTS;
            if (slots_[s].is_valid && slots_[s].seq_num == (next_play_seq_ + i)) {
                buffered += slots_[s].num_frames;
            } else {
                break;
            }
        }

        size_t total_valid_frames = 0;
        for (const auto& s : slots_) {
            if (s.is_valid) {
                total_valid_frames += s.num_frames;
            }
        }

        // The non-contiguous escape is only for steady-state (lossy networks);
        // during the startup fill it must accumulate twice the target so holes
        // in the early delivery can't start the playhead into a dry-out. The
        // escape bypasses the stability gate so real packet loss can't leave
        // the stream stuck in silence.
        const size_t escape_target = startup_pending_ ? (fill_target * 2) : fill_target;
        const bool delivery_stable = hole_free_run_ >= kStableArrivalCount;

        // The contiguous path requires the stability gate; the non-contiguous
        // escape bypasses it (it is the fallback for lossy networks, where a
        // gap-free run may never accumulate and staying silent would be worse).
        if ((buffered >= fill_target && delivery_stable) ||
            total_valid_frames >= escape_target) {
            is_buffering_ = false;
            if (startup_pending_) {
                startup_pending_ = false;
                startup_complete_ms_ = get_time_ms();
            }
            LOG_DEBUG("JitterBuffer: Pre-buffering complete (" << buffered << " frames, "
                      << (config_.sample_rate > 0 ? (buffered * 1000 / config_.sample_rate) : 0) << " ms). Starting playback.");
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

    // Drain-to-target: in steady state (startup prefill done, past the
    // startup grace, not re-buffering), if the buffered level is well above
    // the configured target, shed the excess gently - a burst/stall left the
    // buffer high and it would otherwise stay high forever (constant added
    // latency).
    if (!startup_pending_ && config_.sample_rate > 0) {
        const uint64_t now_ms = get_time_ms();
        const bool past_grace =
            startup_complete_ms_ == 0 || now_ms - startup_complete_ms_ >= kDrainStartGraceMs;
        if (past_grace) {
            const size_t buffered = available_frames_unlocked();
            const size_t margin =
                (static_cast<uint64_t>(config_.sample_rate) * kDrainMarginMs) / 1000;
            if (buffered > target_buffer_frames_ + margin) {
                // Gentle catch-up for small excesses (clock skew, ~1-2 ms/s):
                // tiny 5 ms skips every 400 ms are near-inaudible. A delivery
                // stall (USB tunnel bursts can add ~150 ms) leaves a much
                // larger backlog; shedding that at the gentle rate would leave
                // the user with a clearly audible ~100 ms of lag for tens of
                // seconds, so large backlogs shed ~4x faster (10 ms skips
                // every 100 ms) - a short burst of minor skips beats a long
                // tail of lag.
                const size_t large_backlog_threshold =
                    target_buffer_frames_ + (static_cast<uint64_t>(config_.sample_rate) * 100) / 1000;
                const bool large = buffered > large_backlog_threshold;
                const uint64_t interval = large ? kDrainFastIntervalMs : kDrainIntervalMs;
                const size_t chunk = large ? kDrainFastChunkFrames : kDrainChunkFrames;
                if (now_ms - last_drain_ms_ >= interval) {
                    const size_t excess = buffered - target_buffer_frames_ - margin;
                    const size_t drop = std::min(excess, chunk);
                    advance_playhead_locked(drop);
                    stats_.drained_frames += drop;
                    last_drain_ms_ = now_ms;
                }
            }
        }
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
                slot.pcm_data.clear();
                slot_frame_offset_ = 0;
                next_play_seq_++;
            }
        } else {
            // Check if any valid future packets exist in the jitter buffer
            bool has_future_packets = false;
            for (size_t i = 1; i < MAX_SLOTS; ++i) {
                size_t s = (next_play_seq_ + i) % MAX_SLOTS;
                if (slots_[s].is_valid) {
                    has_future_packets = true;
                    break;
                }
            }

            if (has_future_packets) {
                // Packet loss or gap! Conceal loss with silence / interpolation
                stats_.packets_lost++;
                stats_.underruns++;

                size_t needed = num_frames - frames_delivered;
                size_t conceal_frames = std::min(needed, static_cast<size_t>(config_.frames_per_packet > 0 ? config_.frames_per_packet : 240));

                int16_t* dst_ptr = dest + (frames_delivered * config_.channels);
                std::fill(dst_ptr, dst_ptr + (conceal_frames * config_.channels), 0);

                frames_delivered += conceal_frames;
                slot.is_valid = false;
                slot.pcm_data.clear();
                slot_frame_offset_ = 0;
                next_play_seq_++; // Skip the missing packet slot
            } else {
                // No future packets exist -> buffer ran dry (underrun / pause).
                // Enter buffering mode and output silence; the stability gate
                // in push_packet restarts playback only once delivery is steady
                // again, so the playhead won't immediately dry out a second time.
                is_buffering_ = true;
                stats_.underruns++;
                LOG_DEBUG("JitterBuffer: buffer ran dry (underrun #" << stats_.underruns
                          << "); re-buffering until delivery stabilizes");

                size_t needed = num_frames - frames_delivered;
                int16_t* dst_ptr = dest + (frames_delivered * config_.channels);
                std::fill(dst_ptr, dst_ptr + (needed * config_.channels), 0);
                frames_delivered += needed;
                break;
            }
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

void JitterBuffer::advance_playhead_locked(size_t frames) {
    while (frames > 0) {
        const size_t slot_idx = next_play_seq_ % MAX_SLOTS;
        auto& slot = slots_[slot_idx];
        if (slot.is_valid && slot.seq_num == next_play_seq_) {
            const size_t remaining = slot.num_frames - slot_frame_offset_;
            if (frames < remaining) {
                slot_frame_offset_ += frames;
                frames = 0;
            } else {
                frames -= remaining;
                slot.is_valid = false;
                slot.pcm_data.clear();
                slot_frame_offset_ = 0;
                ++next_play_seq_;
            }
        } else {
            // Hole (missing packet): the playhead skips it without counting
            // against the frames to drop (it is already a gap).
            slot.is_valid = false;
            slot_frame_offset_ = 0;
            ++next_play_seq_;
        }
    }
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
