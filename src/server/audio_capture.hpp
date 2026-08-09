#pragma once

#include "../common/audio_types.hpp"
#include <functional>
#include <memory>
#include <string>

namespace audiorouter {

class IAudioCapture {
public:
    using AudioCallback = std::function<void(const void* data, size_t num_frames, const AudioConfig& config)>;

    virtual ~IAudioCapture() = default;

    virtual bool start(const AudioConfig& desired_config, AudioConfig& actual_config) = 0;
    virtual void stop() = 0;
    virtual bool is_capturing() const = 0;
    virtual void set_audio_callback(AudioCallback cb) = 0;
    virtual std::string get_device_name() const = 0;
};

} // namespace audiorouter
