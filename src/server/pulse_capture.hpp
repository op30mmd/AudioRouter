#pragma once

#include "audio_capture.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace audiorouter {

// Captures system audio output on Linux via PulseAudio / PipeWire-Pulse
// (recording from the default output sink's `.monitor` source, or an explicit
// source specified via `-d pulse:<source>`).
//
// Uses runtime dynamic loading (`dlopen` on `libpulse-simple.so.0` and
// `libpulse.so.0`) so the server builds cleanly on any Linux host without
// requiring PulseAudio development headers.
class PulseCapture : public IAudioCapture {
public:
    explicit PulseCapture(std::string source_spec = "");
    ~PulseCapture() override;

    bool start(const AudioConfig& desired_config, AudioConfig& actual_config) override;
    void stop() override;
    bool is_capturing() const override;
    void set_audio_callback(AudioCallback cb) override;
    std::string get_device_name() const override;

    // Runtime capability probes.
    static bool is_supported();
    static bool server_available();
    static std::string resolve_server_address();
    static std::string resolve_default_monitor_source(const std::string& server = "");
    static std::string parse_source_spec(const std::string& device_spec);
    static std::vector<std::string> list_sources();

private:
    void capture_thread_func();

    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::string source_spec_;
    std::atomic<bool> is_running_;
    std::thread capture_thread_;
    AudioCallback callback_;
    std::mutex callback_mutex_;
    AudioConfig actual_config_;
    std::string device_name_;
};

} // namespace audiorouter
