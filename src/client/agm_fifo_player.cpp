#include "agm_fifo_player.hpp"
#include "android_helpers.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <cstdio>

namespace {

// agmplay's RIFF chunk IDs (agmplay.c: ID_RIFF/ID_FMT/ID_DATA).
constexpr uint32_t kIdRiff = 0x46464952;
constexpr uint32_t kIdWave = 0x45564157;
constexpr uint32_t kIdFmt = 0x20746d66;
constexpr uint32_t kIdData = 0x61746164;

// FIFO used to stream PCM into the agmplay subprocess.
constexpr const char* kFifoPath = "/data/local/tmp/audiorouter_agm.fifo";
constexpr const char* kAgmplayPath = "/vendor/bin/agmplay";
constexpr unsigned int kAgmCard = 100;
constexpr unsigned int kAgmDevice = 100;

// How long to wait for agmplay to open the FIFO before giving up.
constexpr int kFifoReaderTimeoutMs = 10000;
// How long to poll for FIFO writability on each write attempt.
constexpr int kWritePollMs = 50;
// A FIFO that stays full for this long means agmplay stopped consuming
// (usually because another sound preempted the AGM graph on the DSP).
constexpr uint64_t kStallTriggerMs = 2500;
// Minimum gap between agmplay respawns (a preempting sound can last a while;
// retry on a slow cadence instead of thrashing).
constexpr uint64_t kMinRecoverIntervalMs = 3000;
// How long a recovery wait for the fresh agmplay to open the FIFO may take.
constexpr int kRecoveryReaderTimeoutMs = 8000;
// The logcat watcher treats a "session_close" log within this window after
// the HAL touched our backend as OUR session being closed (preemption).
constexpr uint64_t kSessionCloseWindowMs = 2000;
// How long to wait (after the audio HAL pid changes) for the HAL to finish
// re-initializing before respawning agmplay.
constexpr int kHalRestartSettleMs = 800;

} // namespace

namespace audiorouter {

AgmFifoPlayer::AgmFifoPlayer() : backend_("CODEC_DMA-LPAIF_RXTX-RX-1") {}

AgmFifoPlayer::~AgmFifoPlayer() {
    close();
}

bool AgmFifoPlayer::open(const AudioConfig& config, const std::string& device_name) {
#if defined(__linux__) || defined(__ANDROID__)
    if (is_open_) close();

    config_ = config;
    backend_ = "CODEC_DMA-LPAIF_RXTX-RX-1";
    const std::string name = device_name.empty() ? "agm" : device_name;
    if (name.rfind("agm:", 0) == 0) {
        std::string rest = name.substr(4);
        if (!rest.empty()) backend_ = rest;
    }

    // Route the codec to the speaker before agmplay starts (best effort).
    AndroidHelpers::apply_speaker_routing();

    // Mark open before spawning so respawn_subprocess()'s shutdown guard
    // (which kills the child when is_open_ went false) works on the initial
    // open path too.
    is_open_ = true;

    if (!respawn_subprocess()) {
        LOG_ERROR("AgmFifoPlayer: failed to spawn agmplay (is /vendor/bin/agmplay present?)");
        close();
        return false;
    }

    LOG_INFO("AgmFifoPlayer: streaming into agmplay (pid " << agmplay_pid_ << ", backend '"
             << backend_ << "')");
    start_logcat_watcher();
    return true;
#else
    (void)config;
    (void)device_name;
    LOG_INFO("AgmFifoPlayer: AGM playback not available on this platform");
    return false;
#endif
}

bool AgmFifoPlayer::respawn_subprocess() {
    // (Re)create the FIFO: a fresh agmplay needs a fresh path to open.
    ::unlink(kFifoPath);
    if (::mkfifo(kFifoPath, 0666) != 0 && errno != EEXIST) {
        LOG_ERROR("AgmFifoPlayer: mkfifo(" << kFifoPath << ") failed: " << std::strerror(errno));
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        LOG_ERROR("AgmFifoPlayer: fork() failed: " << std::strerror(errno));
        ::unlink(kFifoPath);
        return false;
    }

    if (pid == 0) {
        // Child: exec agmplay. Only async-signal-safe calls are allowed here.
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        const char* envp[] = {"LD_LIBRARY_PATH=/vendor/lib64", nullptr};
        const char* args[] = {kAgmplayPath, kFifoPath, "-D", "100", "-d", "100",
                              "-i", backend_.c_str(), nullptr};
        ::execve(kAgmplayPath, const_cast<char* const*>(args), const_cast<char* const*>(envp));
        _exit(127);  // exec failed
    }

    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (!is_open_) {
            // Shutdown raced the spawn; don't leave an orphan agmplay behind.
            ::kill(pid, SIGKILL);
            int status = 0;
            ::waitpid(pid, &status, 0);
            return false;
        }
        agmplay_pid_ = pid;
    }
    LOG_INFO("AgmFifoPlayer: spawned " << kAgmplayPath << " (pid " << pid << ", backend '"
             << backend_ << "'), waiting for it to open " << kFifoPath << "...");

    if (!wait_for_reader(kRecoveryReaderTimeoutMs)) {
        LOG_ERROR("AgmFifoPlayer: agmplay (pid " << pid << ") did not open the FIFO within "
                  << kRecoveryReaderTimeoutMs << " ms");
        return false;
    }

    write_wav_header(config_.sample_rate > 0 ? static_cast<uint32_t>(config_.sample_rate) : 48000);
    return true;
}

void AgmFifoPlayer::recover() {
    if (recovering_.exchange(true)) return;

    const uint64_t now_ms = get_time_ms();
    if (now_ms - last_recover_ms_ < kMinRecoverIntervalMs) {
        // Too soon after the last respawn; the playback loop will trigger us
        // again. Leave the stream in "stalled" mode until then.
        recovering_ = false;
        return;
    }
    last_recover_ms_ = now_ms;

    LOG_WARN("AgmFifoPlayer: recovering AGM stream (killing stuck agmplay pid " << agmplay_pid_ << ")...");

    // Tear down the wedged subprocess and its FIFO. Only brief operations are
    // done under the lock; the slow respawn below runs unlocked so a
    // concurrent close() (shutdown) can still proceed.
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (!is_open_) {
            recovering_ = false;
            return;
        }
        if (fifo_fd_ >= 0) {
            ::close(fifo_fd_);
            fifo_fd_ = -1;
        }
        if (agmplay_pid_ > 0) {
            ::kill(agmplay_pid_, SIGKILL);  // wedged in a binder call; nothing gentler works
            int status = 0;
            ::waitpid(agmplay_pid_, &status, 0);
            agmplay_pid_ = -1;
        }
        ::unlink(kFifoPath);
    }

    // Restart the audio HAL so every stale AGM session (including the one the
    // wedged agmplay never closed) is torn down on the DSP. Without this, the
    // respawned agmplay gets a session that consumes the FIFO but renders no
    // sound.
    restart_audio_hal();

    // The HAL restart can reset the codec mixer, so the speaker route must be
    // re-asserted *after* it (applying it before would get cleared again and
    // the mixer monitor would re-trigger recovery in a loop). The preempting
    // sound may also have flipped it on the way out - re-assert covers both.
    AndroidHelpers::apply_speaker_routing();

    // Close events logged for the old session may still be in flight (the
    // dying HAL logs session_close lines for our own backend); the session
    // close detection is now mixer-based only, but drop the flag anyway so
    // nothing stale can trigger a second recovery.
    session_closed_ = false;

    const bool respawned = respawn_subprocess();

    if (respawned) {
        // The fresh session may have reconfigured the codec again; re-assert
        // the route once more so the mixer monitor sees an intact path.
        AndroidHelpers::apply_speaker_routing();
        stall_start_ms_ = 0;
        LOG_INFO("AgmFifoPlayer: AGM stream recovered (agmplay pid " << agmplay_pid_ << ")");
    } else {
        LOG_WARN("AgmFifoPlayer: agmplay respawn failed; will retry shortly");
    }
    recovering_ = false;
}

void AgmFifoPlayer::start_logcat_watcher() {
#if defined(__ANDROID__)
    int fds[2];
    if (::pipe(fds) != 0) {
        LOG_WARN("AgmFifoPlayer: pipe() failed: " << std::strerror(errno)
                 << "; AGM session monitoring disabled");
        return;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        LOG_WARN("AgmFifoPlayer: fork() failed: " << std::strerror(errno)
                 << "; AGM session monitoring disabled");
        ::close(fds[0]);
        ::close(fds[1]);
        return;
    }
    if (pid == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::close(fds[1]);
        ::execlp("logcat", "logcat", "-v", "brief", "-s", "AGM", nullptr);
        _exit(127);
    }
    ::close(fds[1]);
    watcher_fd_ = fds[0];
    logcat_pid_ = pid;
    const int flags = ::fcntl(watcher_fd_, F_GETFL, 0);
    ::fcntl(watcher_fd_, F_SETFL, flags | O_NONBLOCK);
    stop_watcher_ = false;
    logcat_thread_ = std::thread([this] { logcat_watch_loop(backend_); });
    LOG_INFO("AgmFifoPlayer: logcat watcher started (pid " << pid << ")");
#endif
}

void AgmFifoPlayer::stop_logcat_watcher() {
    stop_watcher_ = true;
    if (logcat_pid_ > 0) {
        ::kill(logcat_pid_, SIGKILL);
        logcat_pid_ = -1;
    }
    if (logcat_thread_.joinable()) logcat_thread_.join();
    if (watcher_fd_ >= 0) {
        ::close(watcher_fd_);
        watcher_fd_ = -1;
    }
}

void AgmFifoPlayer::logcat_watch_loop(std::string watched_backend) {
    std::string pending;
    char buf[1024];
    uint64_t last_mixer_check_ms = 0;
    while (!stop_watcher_.load()) {
        // The logcat pipe is only drained (never parsed): the HAL logs
        // session_close lines for our own backend while WE kill it during
        // recovery, so any session-close detection based on these lines
        // re-triggers recovery against the fresh stream. The codec mixer
        // state below is the reliable preemption signal.
        (void)watched_backend;
        struct pollfd pfd = {watcher_fd_, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 200);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            const ssize_t n = ::read(watcher_fd_, buf, sizeof(buf));
            if (n > 0) {
                pending.append(buf, static_cast<size_t>(n));
            } else if (n == 0) {
                break;  // logcat exited
            }
        }
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            pending.erase(0, nl + 1);
        }

        // The preempting sound reconfigures the codec, clearing the speaker
        // path controls we set. A cleared route is a reliable early sign of
        // preemption even when the HAL logs nothing (the DSP keeps consuming
        // the graph, so the FIFO never fills).
        const uint64_t now_ms = get_time_ms();
        if (now_ms - last_mixer_check_ms >= 1000) {
            last_mixer_check_ms = now_ms;
            const uint64_t last_recover = last_recover_ms_.load();
            const bool settling = last_recover != 0 &&
                                  now_ms - last_recover < kSessionCloseWindowMs;
            if (!settling && is_open_.load() && !recovering_.load() &&
                !AndroidHelpers::speaker_routing_intact()) {
                session_closed_ = true;
            }
        }
    }
}

std::string AgmFifoPlayer::read_android_prop(const char* prop) {
#if defined(__ANDROID__)
    std::string value;
    const std::string cmd = std::string("getprop ") + prop;
    std::lock_guard<std::mutex> lock(AndroidHelpers::subprocess_mutex());
    if (FILE* f = ::popen(cmd.c_str(), "r")) {
        char buf[128];
        if (::fgets(buf, static_cast<int>(sizeof(buf)), f)) value = buf;
        ::pclose(f);
    }
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) {
        value.pop_back();
    }
    return value;
#else
    (void)prop;
    return "";
#endif
}

void AgmFifoPlayer::restart_audio_hal() {
#if defined(__ANDROID__)
    const std::string old_pid = read_android_prop("init.svc_debug_pid.vendor.audio-hal");
    bool digits = !old_pid.empty();
    for (const char c : old_pid) {
        if (c < '0' || c > '9') {
            digits = false;
            break;
        }
    }
    if (!digits) {
        LOG_WARN("AgmFifoPlayer: cannot determine audio HAL pid ('" << old_pid
                 << "'); skipping HAL restart");
        return;
    }
    if (old_pid == restarted_hal_pid_) {
        // This HAL generation was already restarted by us; killing it again
        // keeps resetting the DSP so every fresh agmplay dies racing the HAL
        // coming up. Just wait for readiness and let the respawn retry.
        LOG_INFO("AgmFifoPlayer: HAL already restarted (pid " << old_pid
                 << "); not killing it again");
        sleep_ms(kHalRestartSettleMs);
        return;
    }
    LOG_WARN("AgmFifoPlayer: restarting vendor.audio-hal (pid " << old_pid
             << ") to clear stale AGM sessions");
    {
        std::lock_guard<std::mutex> lock(AndroidHelpers::subprocess_mutex());
        ::system(("kill -9 " + old_pid).c_str());
    }
    std::string new_pid;
    for (int i = 0; i < 30; ++i) {  // up to 3 s for init to respawn the HAL
        sleep_ms(100);
        new_pid = read_android_prop("init.svc_debug_pid.vendor.audio-hal");
        if (!new_pid.empty() && new_pid != old_pid) break;
    }
    if (new_pid.empty() || new_pid == old_pid) {
        LOG_WARN("AgmFifoPlayer: HAL did not respawn; will retry on next recovery");
    } else {
        restarted_hal_pid_ = new_pid;
    }
    sleep_ms(kHalRestartSettleMs);
#endif
}

bool AgmFifoPlayer::wait_for_reader(int timeout_ms) {
#if defined(__linux__) || defined(__ANDROID__)
    int fd = -1;
    for (int waited = 0; waited < timeout_ms; waited += 100) {
        fd = ::open(kFifoPath, O_WRONLY | O_NONBLOCK);
        if (fd >= 0) break;
        if (errno != ENXIO) {
            LOG_ERROR("AgmFifoPlayer: open(" << kFifoPath << ") failed: " << std::strerror(errno));
            return false;
        }
        sleep_ms(100);
    }
    if (fd < 0) return false;

    // Keep O_NONBLOCK: write_frames paces itself with poll() so a stalled
    // agmplay can never wedge the playback thread in a blocking write.
    fifo_fd_ = fd;
    return true;
#else
    (void)timeout_ms;
    return false;
#endif
}

void AgmFifoPlayer::write_wav_header(uint32_t sample_rate) {
    // Minimal RIFF/WAVE header (44 bytes, no extensions): agmplay parses it
    // with sequential freads and only seeks when the fmt chunk is larger than
    // 16 bytes, so streaming it through the FIFO works.
    struct WavHeader {
        uint32_t riff_id;
        uint32_t riff_sz;
        uint32_t wave_id;
        uint32_t fmt_id;
        uint32_t fmt_sz;
        uint16_t audio_format;
        uint16_t num_channels;
        uint32_t sample_rate;
        uint32_t byte_rate;
        uint16_t block_align;
        uint16_t bits_per_sample;
        uint32_t data_id;
        uint32_t data_sz;
    } hdr;

    hdr.riff_id = kIdRiff;
    hdr.riff_sz = 0xFFFFFFFFu;  // unknown/streamed size
    hdr.wave_id = kIdWave;
    hdr.fmt_id = kIdFmt;
    hdr.fmt_sz = 16;
    hdr.audio_format = 1;  // PCM
    hdr.num_channels = 1;  // mono speaker backend
    hdr.sample_rate = sample_rate;
    hdr.byte_rate = sample_rate * 2;  // 1 ch x 16-bit
    hdr.block_align = 2;
    hdr.bits_per_sample = 16;
    hdr.data_id = kIdData;
    hdr.data_sz = 0xFFFFFFFFu;  // unknown/streamed size

    ssize_t n = ::write(fifo_fd_, &hdr, sizeof(hdr));
    if (n != static_cast<ssize_t>(sizeof(hdr))) {
        if (n < 0 && errno == EAGAIN) {
            // FIFO momentarily full (agmplay still settling in): poll for
            // writability and retry a few times before giving up.
            for (int attempt = 0; attempt < 40; ++attempt) {
                struct pollfd pfd = {fifo_fd_, POLLOUT, 0};
                const int pr = ::poll(&pfd, 1, 50);
                if (pr > 0 && (pfd.revents & POLLOUT)) {
                    const ssize_t n2 = ::write(fifo_fd_, &hdr, sizeof(hdr));
                    if (n2 == static_cast<ssize_t>(sizeof(hdr))) return;
                    if (n2 < 0 && errno == EAGAIN) continue;
                    n = n2;
                    break;
                }
                if (pr < 0 && errno != EINTR) break;
            }
        }
        LOG_ERROR("AgmFifoPlayer: failed to write WAV header to FIFO: "
                  << (n < 0 ? std::strerror(errno) : "short write"));
    }
}

void AgmFifoPlayer::close() {
#if defined(__linux__) || defined(__ANDROID__)
    stop_logcat_watcher();
    std::lock_guard<std::mutex> lock(io_mutex_);
    is_open_ = false;
    if (fifo_fd_ >= 0) {
        // EOF on the FIFO makes agmplay's read loop finish and the session
        // shut down cleanly (pcm_stop + disconnect + pcm_close).
        ::close(fifo_fd_);
        fifo_fd_ = -1;
    }
    if (agmplay_pid_ > 0) {
        for (int i = 0; i < 50; ++i) {  // up to ~1 s for a clean exit
            int status = 0;
            const pid_t r = ::waitpid(agmplay_pid_, &status, WNOHANG);
            if (r == agmplay_pid_) {
                agmplay_pid_ = -1;
                break;
            }
            sleep_ms(20);
        }
        if (agmplay_pid_ > 0) {
            // Backstop: agmplay handles SIGINT for a graceful stop; escalate.
            ::kill(agmplay_pid_, SIGINT);
            for (int i = 0; i < 50 && agmplay_pid_ > 0; ++i) {
                int status = 0;
                const pid_t r = ::waitpid(agmplay_pid_, &status, WNOHANG);
                if (r == agmplay_pid_) {
                    agmplay_pid_ = -1;
                    break;
                }
                sleep_ms(20);
            }
            if (agmplay_pid_ > 0) {
                ::kill(agmplay_pid_, SIGKILL);
                int status = 0;
                ::waitpid(agmplay_pid_, &status, 0);
                agmplay_pid_ = -1;
            }
        }
    }
    ::unlink(kFifoPath);
    LOG_INFO("AgmFifoPlayer: agmplay subprocess stopped");
#endif
}

bool AgmFifoPlayer::is_open() const {
    return is_open_;
}

size_t AgmFifoPlayer::write_frames(const void* pcm_data, size_t num_frames) {
    if (!pcm_data || num_frames == 0) return 0;
#if defined(__linux__) || defined(__ANDROID__)
    const size_t in_channels = (config_.channels > 0) ? config_.channels : 2;
    const int16_t* src = reinterpret_cast<const int16_t*>(pcm_data);

    // Downmix stereo to mono for the speaker backend.
    if (in_channels == 2) {
        downmix_buffer_.resize(num_frames);
        for (size_t i = 0; i < num_frames; ++i) {
            const int32_t l = src[i * 2];
            const int32_t r = src[i * 2 + 1];
            downmix_buffer_[i] = static_cast<int16_t>((l + r) / 2);
        }
        src = downmix_buffer_.data();
    }

    const size_t bytes = num_frames * 2;  // mono S16
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (!is_open_ || fifo_fd_ < 0) return 0;
        // While a recovery is in progress, drop frames so the jitter buffer
        // drains instead of blocking.
        if (recovering_.load()) return 0;
    }

    // A preempting sound either closed our AGM session outright (HAL log) or
    // cleared the codec speaker route (mixer check) while agmplay still
    // drains the FIFO. Restart the stream before the silence becomes
    // permanent.
    if (session_closed_.exchange(false)) {
        LOG_WARN("AgmFifoPlayer: AGM stream preempted by another sound; restarting the AGM stream");
        recover();
        return 0;
    }

    size_t written = 0;
    while (written < bytes) {
        ssize_t n;
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            if (!is_open_ || fifo_fd_ < 0) return 0;
            n = ::write(fifo_fd_, reinterpret_cast<const uint8_t*>(src) + written, bytes - written);
        }
        if (n > 0) {
            written += static_cast<size_t>(n);
            stall_start_ms_ = 0;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) {
            // FIFO full: give agmplay a moment to drain. If it stays full,
            // the AGM stream was preempted (e.g. by a notification) and
            // agmplay is wedged in a binder call - restart it.
            struct pollfd pfd = {fifo_fd_, POLLOUT, 0};
            ::poll(&pfd, 1, kWritePollMs);
            if (stall_start_ms_ == 0) stall_start_ms_ = get_time_ms();
            if (get_time_ms() - stall_start_ms_ >= kStallTriggerMs) {
                LOG_WARN("AgmFifoPlayer: FIFO full for " << (get_time_ms() - stall_start_ms_)
                         << " ms (agmplay pid " << agmplay_pid_
                         << " stopped consuming) - restarting the AGM stream");
                recover();
                return 0;
            }
            continue;
        }
        // EPIPE/EOF or other error: agmplay exited (or never started
        // consuming). Restart it.
        LOG_WARN("AgmFifoPlayer: FIFO write failed: "
                 << (n < 0 ? std::strerror(errno) : "EOF")
                 << " (agmplay pid " << agmplay_pid_ << " exited) - restarting the AGM stream");
        recover();
        return 0;
    }
    return num_frames;
#else
    (void)pcm_data;
    (void)num_frames;
    return 0;
#endif
}

size_t AgmFifoPlayer::get_buffer_delay_frames() const {
    // agmplay owns the ring buffer; report none.
    return 0;
}

void AgmFifoPlayer::flush() {
    // agmplay buffers its own data; nothing to flush from this side.
}

std::string AgmFifoPlayer::get_device_name() const {
    return "agm:" + backend_;
}

} // namespace audiorouter
