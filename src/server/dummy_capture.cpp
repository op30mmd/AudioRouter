#include "dummy_capture.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"
#include <cmath>

namespace audiorouter {

DummyCapture::DummyCapture(double tone_frequency_hz)
    : frequency_hz_(tone_frequency_hz), is_running_(false) {}

DummyCapture::~DummyCapture() {
    stop();
}

bool DummyCapture::start(const AudioConfig& desired_config, AudioConfig& actual_config) {
    if (is_running_) {
        actual_config = config_;
        return true;
    }

    config_ = desired_config;
    if (config_.sample_rate == 0) config_.sample_rate = 48000;
    if (config_.channels == 0) config_.channels = 2;
    if (config_.frames_per_packet == 0) config_.frames_per_packet = 480;
    config_.format = AudioSampleFormat::PCM_S16LE;

    actual_config = config_;
    is_running_ = true;
    thread_ = std::thread(&DummyCapture::worker_thread, this);

    LOG_INFO("DummyCapture: Test tone generator started at " << frequency_hz_ << " Hz ("
             << config_.to_string() << ")");
    return true;
}

void DummyCapture::stop() {
    if (!is_running_) return;
    is_running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
    LOG_INFO("DummyCapture: Stopped");
}

bool DummyCapture::is_capturing() const {
    return is_running_;
}

void DummyCapture::set_audio_callback(AudioCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_ = std::move(cb);
}

std::string DummyCapture::get_device_name() const {
    return "Synthetic Tone Generator (" + std::to_string(static_cast<int>(frequency_hz_)) + " Hz)";
}

void DummyCapture::set_frequency(double freq_hz) {
    frequency_hz_ = freq_hz;
}

void DummyCapture::worker_thread() {
    double phase = 0.0;
    const double rate = static_cast<double>(config_.sample_rate);
    const size_t frames = config_.frames_per_packet;
    std::vector<int16_t> buffer(frames * config_.channels);

    while (is_running_) {
        uint64_t start_time = get_time_us();

        for (size_t i = 0; i < frames; ++i) {
            double sample = std::sin(phase) * 0.3; // 30% volume
            phase += 2.0 * 3.14159265358979323846 * frequency_hz_ / rate;
            if (phase > 2.0 * 3.14159265358979323846) {
                phase -= 2.0 * 3.14159265358979323846;
            }
            int16_t s16 = static_cast<int16_t>(sample * 32767.0);
            for (size_t ch = 0; ch < config_.channels; ++ch) {
                buffer[i * config_.channels + ch] = s16;
            }
        }

        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (callback_) {
                callback_(buffer.data(), frames, config_);
            }
        }

        uint64_t elapsed_us = get_time_us() - start_time;
        uint64_t target_us = (static_cast<uint64_t>(frames) * 1000000ULL) / config_.sample_rate;
        if (target_us > elapsed_us) {
            sleep_us(static_cast<uint32_t>(target_us - elapsed_us));
        }
    }
}

} // namespace audiorouter
