#pragma once

#include "audio_capture.hpp"
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>

namespace audiorouter {

class DummyCapture : public IAudioCapture {
public:
    explicit DummyCapture(double tone_frequency_hz = 440.0);
    ~DummyCapture() override;

    bool start(const AudioConfig& desired_config, AudioConfig& actual_config) override;
    void stop() override;
    bool is_capturing() const override;
    void set_audio_callback(AudioCallback cb) override;
    std::string get_device_name() const override;

    void set_frequency(double freq_hz);

private:
    void worker_thread();

    double frequency_hz_;
    std::atomic<bool> is_running_;
    std::thread thread_;
    AudioCallback callback_;
    std::mutex callback_mutex_;
    AudioConfig config_;
};

} // namespace audiorouter
