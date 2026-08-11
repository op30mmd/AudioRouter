#pragma once

#include "audio_player.hpp"
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
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
    // Spawns a fresh agmplay (fork/exec + FIFO reopen + WAV header) and
    // installs it as the active subprocess. Used both by open() and by
    // recover().
    bool respawn_subprocess();
    // Replaces a stalled or dead agmplay with a fresh one so a notification
    // (or any other audio) that preempted the AGM graph doesn't kill the
    // stream for good. Rate-limited; safe to call repeatedly.
    void recover();

    // Logcat watcher + mixer monitor: a preempting sound can kill our AGM
    // session even when agmplay keeps consuming the FIFO (no stall ever builds
    // up), so FIFO monitoring alone cannot see it. Two independent early
    // signals are watched: the audio HAL logging a session close of our
    // backend under the 'AGM' tag, and the codec speaker route controls being
    // cleared. Either one makes write_frames trigger recover().
    void start_logcat_watcher();
    void stop_logcat_watcher();
    void logcat_watch_loop(std::string watched_backend);
    std::string read_android_prop(const char* prop);
    // Restarts vendor.audio-hal so the DSP drops every stale AGM session
    // before agmplay is respawned. This is what makes recovery actually
    // produce sound again (a wedged agmplay's session never dies on its own).
    void restart_audio_hal();

    AudioConfig config_;
    std::string backend_;
    int fifo_fd_ = -1;
    pid_t agmplay_pid_ = -1;
    // The speaker backend is a mono graph; stereo input is downmixed here
    // before it is written to the FIFO.
    std::vector<int16_t> downmix_buffer_;
    // Serializes write_frames against close() and recover().
    std::mutex io_mutex_;
    std::atomic<bool> is_open_{false};
    // FIFO stall detection: timestamp of when writes first started blocking
    // (0 = currently draining fine).
    uint64_t stall_start_ms_ = 0;
    // Timestamp of the last agmplay respawn (rate limit for recover()).
    // Atomic: read by the mixer monitor thread for the post-recovery grace.
    std::atomic<uint64_t> last_recover_ms_{0};
    // Set while a recovery is in progress; write_frames returns 0 meanwhile
    // so the jitter buffer drains instead of blocking.
    std::atomic<bool> recovering_{false};
    // Set by the logcat watcher when our AGM session was closed by another
    // sound while the FIFO was still flowing.
    std::atomic<bool> session_closed_{false};
    // Logcat watcher state.
    std::thread logcat_thread_;
    std::atomic<bool> stop_watcher_{false};
    int watcher_fd_ = -1;
    pid_t logcat_pid_ = -1;
    // HAL pid this player already restarted; restart_audio_hal() skips the
    // kill while it is unchanged so a recovery retry loop doesn't keep
    // cycling the DSP (which makes every fresh agmplay die in a race with the
    // HAL coming up).
    std::string restarted_hal_pid_;
};

} // namespace audiorouter
