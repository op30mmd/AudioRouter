#pragma once

#include "audio_player.hpp"
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <sys/types.h>

namespace audiorouter {

// AGM playback by streaming PCM into the vendor's own agmplay tool through a
// named pipe (FIFO).
//
// agmplay is Qualcomm's AGM test player (vendor binary on the device). It
// opens the AGM virtual card (100/100), registers the backend/stream graph
// keys (GKV) with the AGM daemon over HIDL binder, and owns the ADSP session
// state - everything a raw dlopen-based approach would have to replicate by
// hand. Spawning it as a subprocess sidesteps that entirely: the client
// writes a minimal WAV header plus mono S16 PCM into the FIFO and agmplay
// renders it.
//
// agmplay's WAV parser is seek-free when the fmt chunk size is exactly 16, so
// a 44-byte header streamed through the FIFO works. Closing the write end of
// the FIFO makes agmplay's read loop finish and the session shut down
// cleanly (pcm_stop + disconnect + pcm_close).
//
// Device naming: agm               -> default backend (card 100, device 100)
//                agm:<backend>     -> agmplay -i <backend>
class AgmFifoPlayer : public IAudioPlayer {
public:
    AgmFifoPlayer();
    ~AgmFifoPlayer() override;

    bool open(const AudioConfig& config, const std::string& device_name = "agm") override;
    void close() override;
    bool is_open() const override;

    size_t write_frames(const void* pcm_data, size_t num_frames) override;
    size_t get_buffer_delay_frames() const override;
    void flush() override;
    std::string get_device_name() const override;

private:
    bool wait_for_reader(int timeout_ms);
    void write_wav_header(uint32_t sample_rate);

    AudioConfig config_;
    std::string backend_;
    int fifo_fd_ = -1;
    pid_t agmplay_pid_ = -1;
    // The speaker backend is a mono graph; stereo input is downmixed here
    // before it is written to the FIFO.
    std::vector<int16_t> downmix_buffer_;
    // Serializes write_frames against close().
    std::mutex io_mutex_;
    std::atomic<bool> is_open_{false};
};

} // namespace audiorouter
