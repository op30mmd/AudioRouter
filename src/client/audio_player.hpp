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

    // True for FIFO-style backends (AAudio, AGM) that buffer whatever they are
    // handed: the playback thread must throttle itself against
    // get_buffer_delay_frames(), or the pipe accumulates a permanent backlog.
    // False for backends whose write_frames() blocks until the device consumes
    // the samples (ALSA, PulseAudio) and for sinks with no real device
    // (dummy). Defaults to false so a backend only opts in deliberately.
    //
    // This must be a property of the PLAYER, not of the requested device name:
    // after a fallback the name still refers to the backend the user asked
    // for, not the one that actually opened.
    virtual bool needs_playback_pacing() const { return false; }
};

} // namespace audiorouter
