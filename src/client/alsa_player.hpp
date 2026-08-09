#pragma once

#include "audio_player.hpp"
#include "direct_alsa.hpp"
#include <string>
#include <memory>
#include <vector>

namespace audiorouter {

class AlsaPlayer : public IAudioPlayer {
public:
    AlsaPlayer();
    ~AlsaPlayer() override;

    bool open(const AudioConfig& config, const std::string& device_name = "default") override;
    void close() override;
    bool is_open() const override;

    size_t write_frames(const void* pcm_data, size_t num_frames) override;
    size_t get_buffer_delay_frames() const override;
    void flush() override;
    std::string get_device_name() const override;

    static std::vector<std::string> get_available_devices();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<DirectAlsaPlayer> direct_fallback_;
    bool using_direct_fallback_;
    bool is_open_;
    std::string device_name_;
    AudioConfig config_;

    bool try_open_via_libasound(const AudioConfig& config, const std::string& device_name);
};

} // namespace audiorouter
