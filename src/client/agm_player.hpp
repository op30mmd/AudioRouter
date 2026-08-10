#pragma once

#include "audio_player.hpp"
#include <string>
#include <vector>
#include <atomic>

// tinyalsa handle (opaque vendor type).
struct pcm;

namespace audiorouter {

// Qualcomm AGM playback via tinyalsa's vendor PCM plugin.
//
// Android's AGM stack exposes the speaker backend ("CODEC_DMA-LPAIF_RXTX-RX-1")
// as Card 100 / Device 100. Opening it with tinyalsa's pcm_open() makes the
// loader pull in /vendor/lib64/sound_fx/libagm_pcm_plugin.so, which performs
// the mandatory graph-key registration with the ADSP and manages the AGM
// session itself. Raw agm_session_open() calls bypass that registration and
// the AGM service rejects them with -EPIPE, so this player deliberately uses
// the plugin path (same as the vendor's own agmplay tool).
//
// Prerequisites (handled with graceful failures + log hints):
//   * Process must run as root (uid 0) so the linker permits dlopen() of
//     vendor libraries (run via 'su'). If vendor paths are namespace-blocked,
//     launch with: su -c "LD_LIBRARY_PATH=/vendor/lib64 ./audiorouter_client ..."
//   * The codec must be routed to the speaker: the mixer controls are set via
//     AndroidHelpers::apply_speaker_routing() before the PCM is opened.
//
// Device naming: agm               -> default backend (card 100, device 100)
//                agm:<backend>     -> accepted for compatibility (same card/device)
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
    struct PcmApi;
    bool load_tinyalsa();
    void unload_tinyalsa();

    PcmApi* api_;
    struct pcm* pcm_impl_;
    std::string backend_;
    AudioConfig config_;
    // The speaker backend is a mono graph; stereo input is downmixed here
    // before it is written to the PCM.
    std::vector<int16_t> downmix_buffer_;
    std::atomic<bool> is_open_;
};

} // namespace audiorouter