#pragma once

#include "audio_capture.hpp"
#include <atomic>
#include <thread>
#include <stop_token>
#include <mutex>
#include <vector>

namespace audiorouter {

class DummyCapture : public IAudioCapture {
public:
    explicit DummyCapture(double tone_frequency_hz = 440.0) noexcept;
    ~DummyCapture() override;

    DummyCapture(const DummyCapture&) = delete;
    DummyCapture& operator=(const DummyCapture&) = delete;

    bool start(const AudioConfig& desired_config, AudioConfig& actual_config) override;
    void stop() override;
    [[nodiscard]] bool is_capturing() const noexcept override;
    void set_audio_callback(AudioCallback cb) override;
    [[nodiscard]] std::string get_device_name() const override;

    void set_frequency(double freq_hz) noexcept;

private:
    void worker_thread(std::stop_token st);

    double frequency_hz_;
    std::atomic<bool> is_running_{false};
    std::jthread thread_;
    AudioCallback callback_;
    std::mutex callback_mutex_;
    AudioConfig config_{};
};

} // namespace audiorouter
