#pragma once

#include "audio_capture.hpp"
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>

namespace audiorouter {

class WasapiCapture : public IAudioCapture {
public:
    WasapiCapture();
    ~WasapiCapture() override;

    bool start(const AudioConfig& desired_config, AudioConfig& actual_config) override;
    void stop() override;
    bool is_capturing() const override;
    void set_audio_callback(AudioCallback cb) override;
    std::string get_device_name() const override;

private:
    void capture_thread_func();

    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<bool> is_running_;
    std::thread capture_thread_;
    AudioCallback callback_;
    std::mutex callback_mutex_;
    AudioConfig actual_config_;
    std::string device_name_;
};

} // namespace audiorouter
