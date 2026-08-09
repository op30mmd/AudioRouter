#pragma once

#include <string>
#include <memory>
#include <mutex>

namespace audiorouter {

enum class MuteMethod {
    EndpointMute = 0,   // Standard IAudioEndpointVolume::SetMute(TRUE)
    VolumeZero,         // Sets Master Volume to 0.0 scalar
    Both                // Both mute and volume zero
};

class AudioEndpointControl {
public:
    AudioEndpointControl();
    ~AudioEndpointControl();

    bool init();
    void shutdown();

    // Mutes the PC speaker and remembers previous mute & volume state
    bool mute_pc_speaker(MuteMethod method = MuteMethod::EndpointMute);

    // Restores PC speaker to its exact pre-connection state
    bool unmute_pc_speaker();

    // Manual controls
    bool set_mute(bool mute);
    bool is_muted() const;
    bool set_volume(float volume_0_to_1);
    float get_volume() const;

    bool is_currently_silenced_by_us() const { return is_silenced_by_us_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool is_silenced_by_us_;
    bool prev_mute_state_;
    float prev_volume_level_;
    MuteMethod active_mute_method_;
    mutable std::mutex mutex_;
};

} // namespace audiorouter
