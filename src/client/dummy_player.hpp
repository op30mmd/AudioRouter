#pragma once

#include "audio_player.hpp"
#include <cstdint>
#include <chrono>
#include <thread>

namespace audiorouter {

class DummyPlayer : public IAudioPlayer {
public:
    DummyPlayer();
    ~DummyPlayer() override = default;

    bool open(const AudioConfig& config, const std::string& device_name = "dummy") override;
    void close() override;
    bool is_open() const override;

    size_t write_frames(const void* pcm_data, size_t num_frames) override;
    size_t get_buffer_delay_frames() const override;
    void flush() override;
    std::string get_device_name() const override;

    // The dummy sink is only ever a stand-in for a real device.
    bool is_placeholder() const override { return true; }

private:
    bool is_open_;
    AudioConfig config_;
    std::string device_name_;
    uint64_t total_frames_written_;
};

} // namespace audiorouter
