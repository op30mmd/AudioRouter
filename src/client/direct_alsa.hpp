#pragma once

#include "audio_player.hpp"
#include <string>
#include <vector>
#include <atomic>

namespace audiorouter {

class DirectAlsaPlayer : public IAudioPlayer {
public:
    DirectAlsaPlayer();
    ~DirectAlsaPlayer() override;

    bool open(const AudioConfig& config, const std::string& device_name = "/dev/snd/pcmC0D0p") override;
    // Open exactly this single PCM node (no fallback candidate loop). Used by
    // the device-open supervisor so one hung node can't block the others.
    bool open_candidate_only(const AudioConfig& config, const std::string& candidate);
    void close() override;
    bool is_open() const override;

    size_t write_frames(const void* pcm_data, size_t num_frames) override;
    size_t get_buffer_delay_frames() const override;
    void flush() override;
    std::string get_device_name() const override;

    static std::vector<std::string> enumerate_kernel_pcm_devices();

private:
    bool try_open_candidate(const std::string& candidate, std::string& last_error);
    std::atomic<int> fd_;
    AudioConfig config_;
    std::string device_path_;
    size_t period_size_frames_;
    size_t buffer_size_frames_;
    std::vector<int16_t> staging_buffer_;
    std::atomic<bool> is_open_;
};

} // namespace audiorouter
