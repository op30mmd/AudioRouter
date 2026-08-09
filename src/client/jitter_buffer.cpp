#include "jitter_buffer.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <limits>
#include <span>

namespace audiorouter {

JitterBuffer::JitterBuffer(uint32_t target_latency_ms) noexcept
    : target_latency_ms_(target_latency_ms ? target_latency_ms : 35),
      target_buffer_frames_(0),
      slots_(MAX_SLOTS),
      next_play_seq_(0),
      has_first_packet_(false),
      is_buffering_(true),
      slot_frame_offset_(0),
      last_arrival_timestamp_us_(0),
      jitter_estimate_us_(0.0),
      last_transit_(0) {}

JitterBuffer::JitterBuffer(JitterBuffer&& other) noexcept
    : config_(other.config_),
      target_latency_ms_(other.target_latency_ms_),
      target_buffer_frames_(other.target_buffer_frames_),
      slots_(std::move(other.slots_)),
      next_play_seq_(other.next_play_seq_),
      has_first_packet_(other.has_first_packet_),
      is_buffering_(other.is_buffering_),
      slot_frame_offset_(other.slot_frame_offset_),
      stats_(other.stats_),
      last_arrival_timestamp_us_(other.last_arrival_timestamp_us_),
      jitter_estimate_us_(other.jitter_estimate_us_),
      last_transit_(other.last_transit_) {
    other.slots_.assign(MAX_SLOTS, {});
    other.next_play_seq_ = 0;
    other.has_first_packet_ = false;
    other.is_buffering_ = true;
    other.slot_frame_offset_ = 0;
}

JitterBuffer& JitterBuffer::operator=(JitterBuffer&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lk(mutex_, other.mutex_);
        config_ = other.config_;
        target_latency_ms_ = other.target_latency_ms_;
        target_buffer_frames_ = other.target_buffer_frames_;
        slots_ = std::move(other.slots_);
        next_play_seq_ = other.next_play_seq_;
        has_first_packet_ = other.has_first_packet_;
        is_buffering_ = other.is_buffering_;
        slot_frame_offset_ = other.slot_frame_offset_;
        stats_ = other.stats_;
        last_arrival_timestamp_us_ = other.last_arrival_timestamp_us_;
        jitter_estimate_us_ = other.jitter_estimate_us_;
        last_transit_ = other.last_transit_;
        other.slots_.assign(MAX_SLOTS, {});
        other.next_play_seq_ = 0;
    }
    return *this;
}

void JitterBuffer::configure(const AudioConfig& config, uint32_t target_latency_ms) {
    if (!config.is_valid()) {
        LOG_WARN("JitterBuffer::configure: invalid AudioConfig " << config.to_string());
        // still proceed but clamp
    }
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    target_latency_ms_ = (target_latency_ms > 5) ? target_latency_ms : 5;
    // overflow-safe: (sample_rate * latency) /1000
    uint64_t sr = config_.sample_rate;
    uint64_t frames = (sr * static_cast<uint64_t>(target_latency_ms_)) / 1000ULL;
    if (frames > 100000) frames = 100000; // sanity cap ~2 sec at 48k
    target_buffer_frames_ = static_cast<size_t>(frames);
    reset_unlocked();
}

void JitterBuffer::reset() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    reset_unlocked();
}

void JitterBuffer::reset_unlocked() noexcept {
    for (auto& slot : slots_) {
        slot.is_valid = false;
        slot.num_frames = 0;
        slot.pcm_data.clear();
        slot.pcm_data.shrink_to_fit();
    }
    next_play_seq_ = 0;
    has_first_packet_ = false;
    is_buffering_ = true;
    slot_frame_offset_ = 0;
    last_arrival_timestamp_us_ = 0;
    jitter_estimate_us_ = 0.0;
    last_transit_ = 0;
    // Note: stats intentionally not cleared on reset? Keep cumulative; but we clear per configure.
    // Reset does keep stats for continuity; if want fresh, caller should re-construct.
}

std::expected<bool, std::string> JitterBuffer::push_packet(uint32_t seq_num, uint64_t timestamp_us,
                                                           std::span<const int16_t> pcm) noexcept {
    if (pcm.empty()) return std::unexpected(std::string("empty pcm span"));
    if (pcm.size() > 100000) return std::unexpected(std::string("pcm span unreasonably large"));
    if (config_.channels == 0) return std::unexpected(std::string("not configured: channels==0"));
    if (pcm.size() % config_.channels != 0) return std::unexpected(std::string("pcm size not multiple of channels"));
    size_t frames = pcm.size() / config_.channels;
    if (frames == 0 || frames > 8192) return std::unexpected(std::string("invalid frame count"));

    std::lock_guard<std::mutex> lock(mutex_);
    // delegate to unlocked helper
    bool ok = push_packet_unlocked(seq_num, timestamp_us, pcm);
    return ok;
}

bool JitterBuffer::push_packet(uint32_t seq_num, uint64_t timestamp_us, const void* pcm_data, size_t num_frames) noexcept {
    if (!pcm_data || num_frames == 0) return false;
    if (config_.channels == 0) return false;
    // overflow check
    if (num_frames > 8192) return false;
    size_t total_samples = num_frames * static_cast<size_t>(config_.channels);
    if (total_samples > 100000) return false;
    // ensure pointer is int16_t aligned — but we just treat as bytes
    auto span = std::span<const int16_t>(static_cast<const int16_t*>(pcm_data), total_samples);
    auto res = push_packet(seq_num, timestamp_us, span);
    return res.has_value() && res.value();
}

bool JitterBuffer::push_packet_unlocked(uint32_t seq_num, uint64_t timestamp_us, std::span<const int16_t> pcm) noexcept {
    uint64_t now_us = get_time_us();

    // RFC 3550 jitter estimation — per-instance state, thread-safe via mutex
    if (last_arrival_timestamp_us_ != 0 && timestamp_us != 0) {
        int64_t transit = static_cast<int64_t>(now_us) - static_cast<int64_t>(timestamp_us);
        if (last_transit_ != 0) {
            int64_t d = std::abs(transit - last_transit_);
            jitter_estimate_us_ += (static_cast<double>(d) - jitter_estimate_us_) / 16.0;
        }
        last_transit_ = transit;
    }
    last_arrival_timestamp_us_ = now_us;
    stats_.packets_received++;

    if (!has_first_packet_) {
        has_first_packet_ = true;
        next_play_seq_ = seq_num;
        is_buffering_ = true;
    }

    int32_t diff = static_cast<int32_t>(seq_num - next_play_seq_);
    if (diff < 0) {
        stats_.packets_out_of_order++;
        return false;
    }
    if (diff >= static_cast<int32_t>(MAX_SLOTS)) {
        LOG_WARN("JitterBuffer: sequence jump diff=" << diff << " resync");
        stats_.overruns++;
        next_play_seq_ = seq_num;
        diff = 0;
    }

    size_t slot_idx = seq_num % MAX_SLOTS;
    auto& slot = slots_[slot_idx];
    if (slot.is_valid && slot.seq_num == seq_num) {
        stats_.packets_duplicate++;
        return false;
    }

    size_t total_samples = pcm.size();
    size_t frames = total_samples / (config_.channels ? config_.channels : 1);
    slot.seq_num = seq_num;
    slot.timestamp_us = timestamp_us;
    slot.num_frames = frames;
    slot.pcm_data.assign(pcm.begin(), pcm.end());
    slot.is_valid = true;

    if (is_buffering_) {
        size_t buffered = 0;
        for (size_t i = 0; i < MAX_SLOTS; ++i) {
            uint32_t expected = next_play_seq_ + static_cast<uint32_t>(i);
            size_t s = expected % MAX_SLOTS;
            if (slots_[s].is_valid && slots_[s].seq_num == expected)
                buffered += slots_[s].num_frames;
            else break;
        }
        if (buffered >= target_buffer_frames_) {
            is_buffering_ = false;
            LOG_DEBUG("JitterBuffer: prebuffer " << buffered << " frames (" << (config_.sample_rate ? (buffered*1000/config_.sample_rate):0) << " ms)");
        }
    }
    return true;
}

size_t JitterBuffer::pop_frames(std::span<int16_t> dest) noexcept {
    if (dest.empty()) return 0;
    if (config_.channels == 0) {
        std::ranges::fill(dest, 0);
        return dest.size() / 1;
    }
    size_t channels = config_.channels;
    if (dest.size() % channels != 0) {
        // dest must be multiple of channels — truncate
    }
    size_t num_frames = dest.size() / channels;
    return pop_frames(dest.data(), num_frames);
}

size_t JitterBuffer::pop_frames(int16_t* dest, size_t num_frames) noexcept {
    if (!dest || num_frames == 0) return 0;
    if (config_.channels == 0 || config_.channels > 32) {
        std::fill(dest, dest + num_frames, 0);
        return num_frames;
    }
    // prevent insane request
    if (num_frames > 8192 * 4) {
        // cap
        num_frames = 8192 * 4;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    size_t channels = config_.channels;
    size_t total_samples = num_frames * channels;
    // overflow guard already via cap

    if (is_buffering_ || !has_first_packet_) {
        std::fill(dest, dest + total_samples, 0);
        return num_frames;
    }

    size_t delivered = 0;
    while (delivered < num_frames) {
        size_t slot_idx = next_play_seq_ % MAX_SLOTS;
        auto& slot = slots_[slot_idx];
        if (slot.is_valid && slot.seq_num == next_play_seq_) {
            size_t avail = (slot.num_frames > slot_frame_offset_) ? (slot.num_frames - slot_frame_offset_) : 0;
            size_t need = num_frames - delivered;
            size_t to_copy = std::min(avail, need);
            if (to_copy == 0) {
                // slot empty but valid — advance
                slot.is_valid = false;
                slot_frame_offset_ = 0;
                next_play_seq_++;
                continue;
            }
            const int16_t* src = slot.pcm_data.data() + (slot_frame_offset_ * channels);
            int16_t* dst = dest + (delivered * channels);
            // Use std::copy for type safety (trivially copyable still memcpy optimal)
            std::copy_n(src, to_copy * channels, dst);
            delivered += to_copy;
            slot_frame_offset_ += to_copy;
            if (slot_frame_offset_ >= slot.num_frames) {
                slot.is_valid = false;
                slot.pcm_data.clear();
                slot.pcm_data.shrink_to_fit();
                slot_frame_offset_ = 0;
                next_play_seq_++;
            }
        } else {
            stats_.packets_lost++;
            stats_.underruns++;
            size_t need = num_frames - delivered;
            size_t conceal = std::min(need, config_.frames_per_packet ? static_cast<size_t>(config_.frames_per_packet) : size_t(240));
            int16_t* dst = dest + (delivered * channels);
            std::fill(dst, dst + conceal * channels, 0);
            delivered += conceal;
            slot.is_valid = false;
            slot.pcm_data.clear();
            slot_frame_offset_ = 0;
            next_play_seq_++;
            // avoid infinite loop if conceal==0
            if (conceal == 0) break;
        }
    }
    return delivered;
}

bool JitterBuffer::is_ready() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return !is_buffering_ && has_first_packet_;
}
size_t JitterBuffer::available_frames() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return available_frames_unlocked();
}
size_t JitterBuffer::available_frames_unlocked() const noexcept {
    size_t total = 0;
    for (size_t i = 0; i < MAX_SLOTS; ++i) {
        uint32_t expected = next_play_seq_ + static_cast<uint32_t>(i);
        size_t s = expected % MAX_SLOTS;
        if (slots_[s].is_valid && slots_[s].seq_num == expected)
            total += slots_[s].num_frames;
        else break;
    }
    if (total >= slot_frame_offset_) total -= slot_frame_offset_;
    return total;
}
double JitterBuffer::available_duration_ms() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return available_duration_ms_unlocked();
}
double JitterBuffer::available_duration_ms_unlocked() const noexcept {
    size_t frames = available_frames_unlocked();
    if (config_.sample_rate == 0) return 0.0;
    return (static_cast<double>(frames) * 1000.0) / static_cast<double>(config_.sample_rate);
}
JitterBufferStats JitterBuffer::get_stats() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    JitterBufferStats s = stats_;
    s.current_buffer_ms = available_duration_ms_unlocked();
    s.avg_jitter_ms = jitter_estimate_us_ / 1000.0;
    return s;
}

} // namespace audiorouter
