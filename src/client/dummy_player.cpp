#include "dummy_player.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

namespace audiorouter {

DummyPlayer::DummyPlayer()
    : is_open_(false), device_name_("dummy"), total_frames_written_(0) {}

bool DummyPlayer::open(const AudioConfig& config, const std::string& device_name) {
    config_ = config;
    device_name_ = device_name;
    is_open_ = true;
    total_frames_written_ = 0;
    LOG_INFO("DummyPlayer: Opened dummy audio sink (" << config_.to_string() << ")");
    return true;
}

void DummyPlayer::close() {
    is_open_ = false;
    LOG_INFO("DummyPlayer: Closed. Total frames written: " << total_frames_written_);
}

bool DummyPlayer::is_open() const {
    return is_open_;
}

size_t DummyPlayer::write_frames(const void* pcm_data, size_t num_frames) {
    (void)pcm_data;
    if (!is_open_) return 0;
    total_frames_written_ += num_frames;

    // Simulate real-time audio consumption timing if needed
    if (config_.sample_rate > 0) {
        uint64_t duration_us = (static_cast<uint64_t>(num_frames) * 1000000ULL) / config_.sample_rate;
        sleep_us(static_cast<uint32_t>(duration_us));
    }

    return num_frames;
}

size_t DummyPlayer::get_buffer_delay_frames() const {
    return 0;
}

void DummyPlayer::flush() {}

std::string DummyPlayer::get_device_name() const {
    return device_name_;
}

} // namespace audiorouter
