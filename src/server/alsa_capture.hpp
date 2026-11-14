#pragma once

#include "audio_capture.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace audiorouter {

// Captures PCM audio on Linux via ALSA (`libasound.so.2`, dynamically loaded).
// Useful for ALSA loopback (`hw:Loopback,1,0`) or pure-ALSA systems without
// PulseAudio/PipeWire.
class AlsaCapture : public IAudioCapture {
public:
    explicit AlsaCapture(std::string device_spec = "default");
    ~AlsaCapture() override;

    bool start(const AudioConfig& desired_config, AudioConfig& actual_config) override;
    void stop() override;
    bool is_capturing() const override;
    void set_audio_callback(AudioCallback cb) override;
    std::string get_device_name() const override;

    static bool is_supported();
    static std::string parse_device_spec(const std::string& device_spec);

private:
    void capture_thread_func();

    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::string device_name_;
    std::atomic<bool> is_running_;
    std::thread capture_thread_;
    AudioCallback callback_;
    std::mutex callback_mutex_;
    AudioConfig actual_config_;
};

} // namespace audiorouter
