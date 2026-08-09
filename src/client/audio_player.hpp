#pragma once

#include "../common/audio_types.hpp"
#include <string>
#include <memory>

namespace audiorouter {

class IAudioPlayer {
public:
    virtual ~IAudioPlayer() = default;

    virtual bool open(const AudioConfig& config, const std::string& device_name = "default") = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    // Writes PCM audio frames (interleaved). Returns number of frames written.
    virtual size_t write_frames(const void* pcm_data, size_t num_frames) = 0;

    // Estimated frames buffered in audio hardware / driver
    virtual size_t get_buffer_delay_frames() const = 0;

    virtual void flush() = 0;
    virtual std::string get_device_name() const = 0;
};

} // namespace audiorouter
