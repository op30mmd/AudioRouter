#include "dummy_capture.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"
#include <cmath>
#include <limits>

namespace audiorouter {

DummyCapture::DummyCapture(double tone_frequency_hz) noexcept
    : frequency_hz_(tone_frequency_hz), is_running_(false) {}

DummyCapture::~DummyCapture() {
    stop();
}

bool DummyCapture::start(const AudioConfig& desired_config, AudioConfig& actual_config) {
    if (is_running_.load()) {
        actual_config = config_;
        return true;
    }

    // Validate and clamp
    AudioConfig cfg = desired_config;
    if (cfg.sample_rate < 8000 || cfg.sample_rate > 192000) cfg.sample_rate = 48000;
    if (cfg.channels == 0 || cfg.channels > 32) cfg.channels = 2;
    if (cfg.frames_per_packet == 0 || cfg.frames_per_packet > 8192) cfg.frames_per_packet = 480;
    cfg.format = AudioSampleFormat::PCM_S16LE;
    if (!cfg.is_valid()) {
        // force safe
        cfg.sample_rate = 48000; cfg.channels=2; cfg.frames_per_packet=480;
    }

    config_ = cfg;
    actual_config = config_;
    is_running_.store(true);
    thread_ = std::jthread([this](std::stop_token st){ this->worker_thread(st); });

    LOG_INFO("DummyCapture: Test tone generator started at " << frequency_hz_ << " Hz (" << config_.to_string() << ")");
    return true;
}

void DummyCapture::stop() {
    bool expected = true;
    if (!is_running_.compare_exchange_strong(expected, false)) return;
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
    LOG_INFO("DummyCapture: Stopped");
}

bool DummyCapture::is_capturing() const noexcept {
    return is_running_.load();
}

void DummyCapture::set_audio_callback(AudioCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_ = std::move(cb);
}

std::string DummyCapture::get_device_name() const {
    return "Synthetic Tone Generator (" + std::to_string(static_cast<int>(frequency_hz_)) + " Hz)";
}

void DummyCapture::set_frequency(double freq_hz) noexcept {
    if (std::isfinite(freq_hz) && freq_hz > 0 && freq_hz < 20000) {
        frequency_hz_ = freq_hz;
    }
}

void DummyCapture::worker_thread(std::stop_token st) {
    double phase = 0.0;
    const double rate = static_cast<double>(config_.sample_rate ? config_.sample_rate : 48000);
    const size_t frames = config_.frames_per_packet ? config_.frames_per_packet : 480;
    const size_t channels = config_.channels ? config_.channels : 2;
    if (frames==0 || channels==0 || channels>32) return;
    // Prevent overflow: frames*channels fits? Check.
    if (frames > 100000 / channels) return;
    std::vector<int16_t> buffer(frames * channels);

    while (!st.stop_requested() && is_running_.load()) {
        uint64_t start_time = get_time_us();

        for (size_t i = 0; i < frames; ++i) {
            if (st.stop_requested() || !is_running_.load()) break;
            double sample = std::sin(phase) * 0.3;
            phase += 2.0 * 3.14159265358979323846 * frequency_hz_ / rate;
            if (phase > 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;
            // Clamp phase to finite
            if (!std::isfinite(phase)) phase = 0;
            int16_t s16 = static_cast<int16_t>(std::clamp(sample, -1.0, 1.0) * 32767.0);
            for (size_t ch = 0; ch < channels; ++ch) {
                buffer[i * channels + ch] = s16;
            }
        }

        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (callback_ && is_running_.load() && !st.stop_requested()) {
                callback_(buffer.data(), frames, config_);
            }
        }

        uint64_t elapsed_us = get_time_us() - start_time;
        uint64_t target_us = (static_cast<uint64_t>(frames) * 1000000ULL) / static_cast<uint64_t>(rate);
        if (target_us > elapsed_us) {
            uint64_t sleep_needed = target_us - elapsed_us;
            // sleep in chunks to remain responsive to stop
            while (sleep_needed > 0 && !st.stop_requested() && is_running_.load()) {
                uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(sleep_needed, 5000));
                sleep_us(chunk);
                sleep_needed -= chunk;
            }
        } else {
            // yield to avoid busy spin
            std::this_thread::yield();
        }
    }
}

} // namespace audiorouter
