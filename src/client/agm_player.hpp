#pragma once

#include "audio_player.hpp"
#include <string>
#include <mutex>
#include <atomic>

namespace audiorouter {

// Qualcomm Audio Graph Manager (AGM) playback path.
//
// dlopen()s the vendor-supplied libagmclient.so (and, optionally,
// libtinyalsa.so) at runtime and streams PCM to the CODEC_DMA backend of the
// DSP-created graph. Unlike the raw /dev/snd PCM nodes this path does not
// depend on Android's audioserver having released the hardware: AGM talks to
// the vendor's agm_service, which keeps running even with audioserver stopped.
//
// Prerequisites (handled with graceful failures + log hints):
//   * Process must run as root (uid 0) so the linker permits dlopen() of
//     vendor libraries (run via 'su', not plain Termux).
//   * The PCM path on the codec must be routed to the speaker: the mixer
//     controls must be set (AndroidHelpers::apply_speaker_routing()).
//
// Device naming:   agm                        -> default backend
//                  agm:CODEC_DMA-LPAIF_RXTX-RX-1 -> given backend (or
//                                                   libtinyalsa stream name)
// The optional libtinyalsa path (agm:pcmC0D0p style) is NOT implemented; AGM
// backends are identified by their graph backend names.
class AgmPlayer : public IAudioPlayer {
public:
    AgmPlayer();
    ~AgmPlayer() override;

    bool open(const AudioConfig& config, const std::string& device_name = "agm") override;
    void close() override;
    bool is_open() const override;

    size_t write_frames(const void* pcm_data, size_t num_frames) override;
    size_t get_buffer_delay_frames() const override;
    void flush() override;
    std::string get_device_name() const override;

private:
    struct AgmApi;
    bool load_agm_library();
    void unload_agm_library();
    bool open_session(const AudioConfig& config, const std::string& backend);

    AgmApi* api_;
    uint64_t session_handle_;
    std::string backend_;
    AudioConfig config_;
    size_t frame_bytes_;
    std::atomic<bool> is_open_;
};

} // namespace audiorouter