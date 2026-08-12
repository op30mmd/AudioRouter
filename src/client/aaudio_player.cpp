#include "aaudio_player.hpp"
#include "android_helpers.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#if defined(__ANDROID__) && defined(AAUDIO_ENABLED)
#include <aaudio/AAudio.h>
#endif

namespace {

// Set by the SIGTERM handler in the forked AAudio helper process so the pump
// loop exits gracefully on shutdown.
std::atomic<bool> g_child_stop{false};

// The FIFO carries PCM from write_frames() to the AAudio pump thread. It is
// resolved at runtime (see AaudioFifoPlayer::fifo_path()): AAudio must run as
// the non-root Termux user, and that user cannot write /data/local/tmp
// (shell-owned), so the FIFO lives under $HOME when available.

// Expand the pipe capacity to 1 MB (~5 s of 48 kHz stereo audio). The pipe is
// the back-pressure buffer between the network and the AAudio stream; a small
// default (64 KB) would stall on bursty Wi-Fi.
constexpr int kFifoSizeBytes = 1048576;

// How long AAudioStream_write() may block for space in the AAudio buffer.
// 200 ms is far above the ~20 ms a 960-frame chunk needs on a healthy stream,
// but short enough that a stream which opened yet never starts rendering is
// detected quickly (the daemon keeps the original 500 ms).
constexpr int64_t kWriteTimeoutNs = 200000000LL;

// Poll timeouts keep shutdown responsive: the pump polls the FIFO for 100 ms
// at a time and write_frames() waits at most 50 ms for pipe space, both
// re-checking the open/stop flags each iteration.
constexpr int kFifoPollMs = 100;
constexpr int kWritePollMs = 50;

// Minimum gap between AAudio stream recreations (a flapping routing change
// must not thrash stream open/close).
constexpr uint64_t kMinRebuildIntervalMs = 1000;

// How long open() waits for the stream to actually start (requestStart is
// asynchronous) and for the probe write to be consumed before declaring the
// stream wedged.
constexpr int kStartWaitMs = 1000;

// How long the probe waits for the device to consume the probe chunk.
constexpr int kProbeWaitMs = 1000;

// How long the parent waits for the forked AAudio helper to report ready.
constexpr int kHelperReadyTimeoutMs = 10000;

// A stream that fails/stalls this many consecutive writes (~1.6 s at the
// 200 ms write timeout) is considered wedged and gets recreated.
constexpr int kStallRebuildThreshold = 8;

// A stream that keeps stalling even after this many recreations is never
// going to render on this device; give up instead of rebuilding forever.
constexpr int kMaxStallRebuilds = 5;

} // namespace

namespace audiorouter {

AaudioFifoPlayer::AaudioFifoPlayer() = default;

AaudioFifoPlayer::~AaudioFifoPlayer() {
    close();
}

std::string AaudioFifoPlayer::fifo_path() {
    // AAudio runs as the (non-root) Termux user, whose home is writable by
    // the app user; /data/local/tmp is shell-owned and not writable by app
    // uids. Resolution: $HOME (the normal Termux case), else the Termux home
    // (the root-with-helper case - the helper drops to the Termux user, so
    // parent and child agree on the path), else /data/local/tmp.
    static const std::string path = [] {
        if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
            return std::string(home) + "/audiorouter_aaudio.fifo";
        }
        uid_t uid = 0;
        gid_t gid = 0;
        std::string termux_home;
        if (AndroidHelpers::termux_user(&uid, &gid, &termux_home)) {
            return termux_home + "/audiorouter_aaudio.fifo";
        }
        return std::string("/data/local/tmp/audiorouter_aaudio.fifo");
    }();
    return path;
}

bool AaudioFifoPlayer::open(const AudioConfig& config, const std::string& device_name) {
#if defined(__ANDROID__) && defined(AAUDIO_ENABLED)
    if (is_open_.load()) close();

    // Fresh session: reset the stall-recovery state from any previous run.
    consecutive_write_failures_ = 0;
    stall_rebuilds_ = 0;
    deep_retry_ = false;

    config_ = config;
    if (config_.sample_rate == 0) config_.sample_rate = 48000;
    if (config_.channels == 0) config_.channels = 2;

    // Parse "aaudio" / "aaudio:deep" / "aaudio:voip".
    mode_ = "lowlatency";
    device_name_ = device_name.empty() ? "aaudio" : device_name;
    if (device_name_.rfind("aaudio:", 0) == 0) {
        const std::string mode = device_name_.substr(7);
        if (mode == "deep" || mode == "voip" || mode == "lowlatency") {
            mode_ = mode;
        } else {
            LOG_WARN("AaudioFifoPlayer: unknown aaudio mode '" << mode
                     << "' (expected 'deep' or 'voip'); using default low-latency mode");
        }
    }

    // AAudio is attributed to the calling app; UID 0 (root) has no app
    // attribution token, so Android audio policy blocks the AAudio data path
    // for root: the stream opens and reaches STARTED but never renders.
    // When running as root (needed for -b auto / SO_BINDTODEVICE), the stream
    // is opened in a forked helper process that drops to the Termux app user;
    // the parent keeps root so the AGM/ALSA fallback still works. Escape
    // hatch for ROMs that do allow root: AUDIOROUTER_AAUDIO_AS_ROOT=1.
    if (getuid() == 0 && std::getenv("AUDIOROUTER_AAUDIO_AS_ROOT") == nullptr) {
        return open_via_helper();
    }
    return open_in_process();
#else
    (void)config;
    (void)device_name;
    LOG_INFO("AaudioFifoPlayer: AAudio not available on this platform (requires "
             "an Android API 26+ build with libaaudio)");
    return false;
#endif
}

#if defined(__ANDROID__) && defined(AAUDIO_ENABLED)

bool AaudioFifoPlayer::open_in_process() {
    if (!create_fifo()) return false;

    if (!open_stream_and_probe(false)) {
        destroy_fifo();
        return false;
    }

    LOG_INFO("AaudioFifoPlayer: AAudio stream opened: "
             << AAudioStream_getSampleRate(static_cast<AAudioStream*>(stream_))
             << " Hz, " << AAudioStream_getChannelCount(static_cast<AAudioStream*>(stream_))
             << " ch, mode '" << mode_ << "', state "
             << AAudioStream_getState(static_cast<AAudioStream*>(stream_))
             << ", FIFO " << fifo_path());

    stop_pump_.store(false);
    is_open_.store(true);
    pump_thread_ = std::thread(&AaudioFifoPlayer::pump_loop, this);
    return true;
}

bool AaudioFifoPlayer::open_via_helper() {
    // Create the FIFO as root (the helper inherits the open fd), then fork.
    if (!create_fifo()) return false;

    int status_pipe[2];
    if (::pipe(status_pipe) != 0) {
        LOG_ERROR("AaudioFifoPlayer: pipe() failed: " << std::strerror(errno));
        destroy_fifo();
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        LOG_ERROR("AaudioFifoPlayer: fork() failed: " << std::strerror(errno));
        ::close(status_pipe[0]);
        ::close(status_pipe[1]);
        destroy_fifo();
        return false;
    }
    if (pid == 0) {
        // Child: the AAudio stream lives here, as the Termux app user.
        ::close(status_pipe[0]);
        helper_child_main(status_pipe[1]);
        _exit(1);  // not reached
    }

    // Parent: keep root. Wait for the helper to report ready/failed.
    ::close(status_pipe[1]);
    child_pid_ = pid;
    status_fd_ = status_pipe[0];

    bool ready = false;
    char c = 0;
    const uint64_t deadline = get_time_ms() + kHelperReadyTimeoutMs;
    while (get_time_ms() < deadline) {
        struct pollfd pfd = {status_fd_, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 100);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            const ssize_t n = ::read(status_fd_, &c, 1);
            if (n == 1) {
                ready = (c == 'R');
                break;
            }
            if (n == 0) break;  // helper exited without reporting
        }
        if (pr < 0 && errno != EINTR) break;
    }

    if (ready) {
        is_open_.store(true);
        stop_monitor_.store(false);
        monitor_thread_ = std::thread(&AaudioFifoPlayer::monitor_loop, this);
        LOG_INFO("AaudioFifoPlayer: AAudio helper (pid " << pid
                 << ") ready - stream renders as the Termux app user, FIFO " << fifo_path());
        return true;
    }

    LOG_ERROR("AaudioFifoPlayer: AAudio helper failed to open a renderable stream "
              "(no Termux app user to drop to, or the AAudio data path is blocked on this "
              "device). Falling back to AGM/ALSA. Run without '-b auto' and without su for "
              "plain non-root AAudio, or use '-d agm'.");
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    child_pid_ = -1;
    ::close(status_fd_);
    status_fd_ = -1;
    destroy_fifo();
    return false;
}

// Runs in the forked child. Must not use LOG or any mutex that other threads
// may have held at fork time - only fprintf and raw syscalls.
void AaudioFifoPlayer::helper_child_main(int status_fd) {
    // Die with the parent (no orphans).
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);

    // Drop to the Termux app user so the AAudio data path is attributed to a
    // real app (Android blocks AAudio for UID 0).
    uid_t uid = 0;
    gid_t gid = 0;
    std::string home;
    if (!AndroidHelpers::termux_user(&uid, &gid, &home)) {
        std::fprintf(stderr, "AaudioFifoPlayer: helper: no Termux app user found; AAudio "
                             "cannot render under root\n");
        ::write(status_fd, "F", 1);
        _exit(1);
    }
    ::setenv("HOME", home.c_str(), 1);
    if (::setgroups(0, nullptr) != 0 || ::setgid(gid) != 0 || ::setuid(uid) != 0) {
        std::fprintf(stderr, "AaudioFifoPlayer: helper: privilege drop failed: %s\n",
                     std::strerror(errno));
        ::write(status_fd, "F", 1);
        _exit(1);
    }

    g_child_stop.store(false);
    ::signal(SIGTERM, [](int) { g_child_stop.store(true); });

    if (!open_stream_and_probe(true)) {
        ::write(status_fd, "F", 1);
        _exit(1);
    }
    std::fprintf(stderr, "AaudioFifoPlayer: helper (uid %u): AAudio stream ready\n", uid);
    ::write(status_fd, "R", 1);

    // Pump PCM from the inherited FIFO fd into the AAudio stream.
    pump_loop();

    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        if (stream_ != nullptr) {
            AAudioStream_close(static_cast<AAudioStream*>(stream_));
            stream_ = nullptr;
        }
    }
    ::write(status_fd, "D", 1);
    _exit(0);
}

// Watches the helper's status pipe; on 'D' or EOF (helper exited) the AAudio
// stream is gone - mark the player dead so write_frames stops feeding it.
void AaudioFifoPlayer::monitor_loop() {
    char c = 0;
    while (::read(status_fd_, &c, 1) > 0) {
        if (c == 'D') break;
    }
    if (!stop_monitor_.load()) {
        LOG_WARN("AaudioFifoPlayer: AAudio helper exited; marking the player dead");
        mark_dead();
    }
}

void AaudioFifoPlayer::mark_dead() {
    is_open_.store(false);
}

#endif  // __ANDROID__ && AAUDIO_ENABLED

void AaudioFifoPlayer::close() {
#if defined(__ANDROID__) && defined(AAUDIO_ENABLED)
    stop_pump_.store(true);
    stop_monitor_.store(true);

    if (child_pid_ > 0) {
        // Helper mode: stop the helper, unblock the monitor, reap.
        ::kill(child_pid_, SIGTERM);
        if (status_fd_ >= 0) {
            ::close(status_fd_);  // unblocks monitor_loop's read
            status_fd_ = -1;
        }
        if (monitor_thread_.joinable()) monitor_thread_.join();
        for (int i = 0; i < 50 && child_pid_ > 0; ++i) {  // up to ~1 s
            int status = 0;
            if (::waitpid(child_pid_, &status, WNOHANG) == child_pid_) {
                child_pid_ = -1;
                break;
            }
            sleep_ms(20);
        }
        if (child_pid_ > 0) {
            ::kill(child_pid_, SIGKILL);
            int status = 0;
            ::waitpid(child_pid_, &status, 0);
            child_pid_ = -1;
        }
    } else {
        // In-process mode: close the stream FIRST, before joining the pump,
        // so a write stuck on a dead output path is unblocked by the close.
        {
            std::lock_guard<std::mutex> lock(stream_mutex_);
            if (stream_ != nullptr) {
                AAudioStream_close(static_cast<AAudioStream*>(stream_));
                stream_ = nullptr;
            }
        }
        if (pump_thread_.joinable()) pump_thread_.join();
    }

    if (fifo_fd_ >= 0) {
        ::close(fifo_fd_);
        fifo_fd_ = -1;
    }
    ::unlink(fifo_path().c_str());

    frames_in_flight_.store(0);
    is_open_.store(false);
    LOG_INFO("AaudioFifoPlayer: AAudio stream closed");
#else
    is_open_.store(false);
#endif
}

bool AaudioFifoPlayer::is_open() const {
    return is_open_.load();
}

bool AaudioFifoPlayer::is_supported() {
#if defined(__ANDROID__) && defined(AAUDIO_ENABLED)
    return true;
#else
    return false;
#endif
}

size_t AaudioFifoPlayer::write_frames(const void* pcm_data, size_t num_frames) {
    if (pcm_data == nullptr || num_frames == 0) return 0;
    const size_t bytes_per_frame = config_.bytes_per_frame();
    if (bytes_per_frame == 0) return 0;

    const size_t total_bytes = num_frames * bytes_per_frame;
    const auto* src = reinterpret_cast<const uint8_t*>(pcm_data);
    size_t written_bytes = 0;

    while (written_bytes < total_bytes) {
        if (!is_open_.load() || fifo_fd_ < 0) break;
        const ssize_t n = ::write(fifo_fd_, src + written_bytes, total_bytes - written_bytes);
        if (n > 0) {
            written_bytes += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) {
            // Pipe full: wait for the pump thread to drain it. The FIFO's
            // capacity plus AAudio's buffer pace playback, so this back-
            // pressure is what keeps the stream glitch-free.
            struct pollfd pfd = {fifo_fd_, POLLOUT, 0};
            ::poll(&pfd, 1, kWritePollMs);
            continue;
        }
        // EBADF (closed concurrently) or any other error: stop writing.
        break;
    }
    return written_bytes / bytes_per_frame;
}

size_t AaudioFifoPlayer::get_buffer_delay_frames() const {
    if (!is_open_.load()) return 0;
    const size_t bytes_per_frame = config_.bytes_per_frame();

    // PCM waiting in the FIFO right now (pump thread may already hold some).
    int fifo_bytes = 0;
    if (fifo_fd_ >= 0) ::ioctl(fifo_fd_, FIONREAD, &fifo_bytes);
    const size_t fifo_frames =
        (bytes_per_frame > 0 && fifo_bytes > 0) ? static_cast<size_t>(fifo_bytes) / bytes_per_frame : 0;

    // PCM queued inside the AAudio stream itself. Prefer the live counters;
    // if the pump is mid-write, fall back to its last published snapshot.
    // (In helper mode the stream lives in the child process - the FIFO bytes
    // are the only buffer we can see from here.)
    int64_t in_flight = 0;
    if (child_pid_ < 0) {
        in_flight = frames_in_flight_.load();
#if defined(__ANDROID__) && defined(AAUDIO_ENABLED)
        if (stream_mutex_.try_lock()) {
            if (stream_ != nullptr) {
                auto* stream = static_cast<AAudioStream*>(stream_);
                in_flight = AAudioStream_getFramesWritten(stream) - AAudioStream_getFramesRead(stream);
            }
            stream_mutex_.unlock();
        }
#endif
    }
    if (in_flight < 0) in_flight = 0;

    return fifo_frames + static_cast<size_t>(in_flight);
}

void AaudioFifoPlayer::flush() {
    // The FIFO drains in real time (~20 ms of data); there is nothing to
    // force out beyond what the pump already consumes.
}

std::string AaudioFifoPlayer::get_device_name() const {
    return device_name_;
}

#if defined(__ANDROID__) && defined(AAUDIO_ENABLED)

void AaudioFifoPlayer::configure_builder(void* builder_ptr) {
    auto* builder = static_cast<AAudioStreamBuilder*>(builder_ptr);

    // The AudioRouter server always negotiates 48 kHz stereo S16, but honor
    // whatever the server actually sent (AAudio will resample as needed).
    AAudioStreamBuilder_setSampleRate(builder, static_cast<int32_t>(config_.sample_rate));
    AAudioStreamBuilder_setChannelCount(builder, static_cast<int32_t>(config_.channels));

    aaudio_format_t format = AAUDIO_FORMAT_PCM_I16;
    switch (config_.format) {
        case AudioSampleFormat::PCM_FLOAT32LE:
            format = AAUDIO_FORMAT_PCM_FLOAT;
            break;
        case AudioSampleFormat::PCM_S24LE:
        case AudioSampleFormat::PCM_S32LE:
            // I24/I32 formats are API 31+; deliver 24/32-bit content as 16-bit
            // rather than risk the low 8 bits being truncated by AAudio.
            LOG_WARN("AaudioFifoPlayer: format " << to_string_view(config_.format)
                     << " not natively supported; playing as S16");
            break;
        default:
            break;  // PCM_S16LE
    }
    AAudioStreamBuilder_setFormat(builder, format);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);

    // Usage/content-type hint the audio policy how to route/mix the stream
    // (API 28+; below that AAudio defaults to media).
#if __ANDROID_API__ >= 28
    if (mode_ == "voip") {
        AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_VOICE_COMMUNICATION);
        AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_SPEECH);
    } else {
        AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
        AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
    }
#endif
    // Performance mode is available since API 26 and MUST be applied on every
    // build (not just API 28+): it selects the stream path. LOW_LATENCY uses
    // the MMAP path when the HAL supports it; on devices where that path never
    // produces timestamps (every write returns 0), deep_retry_ downgrades to
    // PERFORMANCE_MODE_NONE, which uses the legacy AudioTrack path that runs
    // through the normal mixer.
    AAudioStreamBuilder_setPerformanceMode(builder,
        (deep_retry_ || mode_ == "deep") ? AAUDIO_PERFORMANCE_MODE_NONE
                                         : AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
}

bool AaudioFifoPlayer::wait_for_start_locked(int timeout_ms) {
    if (stream_ == nullptr) return false;
    auto* stream = static_cast<AAudioStream*>(stream_);

    const uint64_t deadline = get_time_ms() + timeout_ms;
    while (get_time_ms() < deadline) {
        const aaudio_stream_state_t state = AAudioStream_getState(stream);
        if (state == AAUDIO_STREAM_STATE_STARTED) {
            return true;  // fully started
        }
        if (state == AAUDIO_STREAM_STATE_DISCONNECTED ||
            state == AAUDIO_STREAM_STATE_CLOSED ||
            state == AAUDIO_STREAM_STATE_UNINITIALIZED) {
            return false;  // dead stream, no point waiting
        }
        // STARTING / OPEN / UNKNOWN: keep (re)issuing the start request and
        // poll. AAudioStream_write returns 0 until the stream is STARTED, so
        // open() must not proceed before this.
        AAudioStream_requestStart(stream);
        sleep_ms(50);
    }
    return false;
}

bool AaudioFifoPlayer::open_stream_and_probe(bool quiet) {
    auto report = [quiet](const char* level, const std::string& msg) {
        if (quiet) {
            std::fprintf(stderr, "AaudioFifoPlayer: %s: %s\n", level, msg.c_str());
        } else if (std::strcmp(level, "WARN") == 0) {
            LOG_WARN("AaudioFifoPlayer: " << msg);
        } else {
            LOG_ERROR("AaudioFifoPlayer: " << msg);
        }
    };

    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        AAudioStreamBuilder* builder = nullptr;
        aaudio_result_t res = AAudio_createStreamBuilder(&builder);
        if (res != AAUDIO_OK || builder == nullptr) {
            report("ERROR", "failed to create AAudio stream builder: " +
                                std::string(AAudio_convertResultToText(res)));
            return false;
        }
        configure_builder(builder);
        AAudioStream* opened = nullptr;
        res = AAudioStreamBuilder_openStream(builder, &opened);
        AAudioStreamBuilder_delete(builder);
        if (res != AAUDIO_OK) {
            report("ERROR", "failed to open AAudio stream: " +
                                std::string(AAudio_convertResultToText(res)));
            return false;
        }
        stream_ = opened;
        AAudioStream_requestStart(opened);
    }

    // requestStart() is asynchronous. Wait for STARTED, then verify the data
    // path actually renders: a stream that opens but never consumes (dead
    // MMAP path, root-UID block, wedged HAL session) would otherwise eat
    // write timeouts forever while the FIFO fills and the jitter buffer
    // overflows. Retry once with the deep-buffer performance mode (legacy
    // AudioTrack path) before giving up.
    bool ready = false;
    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        ready = wait_for_start_locked(kStartWaitMs);
    }
    ready = ready && probe_stream_ready();
    if (!ready) {
        report("WARN", "stream did not start rendering within " + std::to_string(kStartWaitMs) +
                           " ms; retrying with deep-buffer performance mode");
        {
            std::lock_guard<std::mutex> lock(stream_mutex_);
            deep_retry_ = true;
            rebuild_stream_locked();
            ready = stream_ != nullptr && wait_for_start_locked(kStartWaitMs);
        }
        ready = ready && probe_stream_ready();
    }
    if (!ready) {
        report("ERROR",
               "AAudio cannot render on this device (the stream starts but the audio data "
               "path never runs - the HAL/MMAP output is not consuming). Falling back to "
               "AGM/ALSA. You can also try '-d agm', restart Android audio ('stop "
               "audioserver && start audioserver'), or reboot the device.");
        {
            std::lock_guard<std::mutex> lock(stream_mutex_);
            if (stream_ != nullptr) {
                AAudioStream_close(static_cast<AAudioStream*>(stream_));
                stream_ = nullptr;
            }
        }
        return false;
    }
    return true;
}

bool AaudioFifoPlayer::probe_stream_ready() {
    std::lock_guard<std::mutex> lock(stream_mutex_);
    if (stream_ == nullptr) return false;
    auto* stream = static_cast<AAudioStream*>(stream_);

    // Write one small chunk of silence and watch for consumption. This is the
    // correct readiness test for BOTH paths: on the MMAP path a dead output
    // returns 0 from the write (or never advances framesRead), while on the
    // legacy AudioTrack path the device only starts consuming once data is
    // actually written - so probing timestamps on an idle stream would
    // falsely reject a healthy legacy stream.
    const size_t bytes_per_frame = config_.bytes_per_frame();
    if (bytes_per_frame == 0) return false;
    constexpr int32_t kProbeFrames = 240;  // 5 ms
    std::vector<int16_t> silence(static_cast<size_t>(kProbeFrames) * config_.channels, 0);
    const aaudio_result_t written =
        AAudioStream_write(stream, silence.data(), kProbeFrames, kWriteTimeoutNs);
    if (written <= 0) return false;

    const uint64_t deadline = get_time_ms() + kProbeWaitMs;
    while (get_time_ms() < deadline) {
        if (AAudioStream_getFramesRead(stream) > 0) return true;  // device consumed
        int64_t position = 0;
        int64_t time_ns = 0;
        if (AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &position, &time_ns) == AAUDIO_OK &&
            time_ns > 0) {
            return true;  // data path is producing timestamps
        }
        sleep_ms(50);
    }
    return false;
}

// Restarts the stream when it is not running (underrun, pause, stop).
// Returns false when the stream is gone for good (disconnect during rebuild).
bool AaudioFifoPlayer::ensure_stream_started_locked() {
    if (stream_ == nullptr) return false;
    auto* stream = static_cast<AAudioStream*>(stream_);

    const aaudio_stream_state_t state = AAudioStream_getState(stream);
    if (state == AAUDIO_STREAM_STATE_DISCONNECTED) {
        // Routing changed (headphones unplugged, BT reconnect, ...): the old
        // stream is dead; recreate it from the builder config.
        const uint64_t now_ms = get_time_ms();
        if (now_ms - last_rebuild_ms_ >= kMinRebuildIntervalMs) {
            last_rebuild_ms_ = now_ms;
            LOG_WARN("AaudioFifoPlayer: stream disconnected (routing change); recreating AAudio stream");
            rebuild_stream_locked();
        }
        return stream_ != nullptr;
    }

    // Auto-restart the stream if it underran / was paused / stopped.
    if (state != AAUDIO_STREAM_STATE_STARTED && state != AAUDIO_STREAM_STATE_STARTING) {
        AAudioStream_requestStart(stream);
    }
    return true;
}

bool AaudioFifoPlayer::rebuild_stream_locked() {
    if (stream_ != nullptr) {
        AAudioStream_close(static_cast<AAudioStream*>(stream_));
        stream_ = nullptr;
    }
    frames_in_flight_.store(0);
    consecutive_write_failures_ = 0;

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t res = AAudio_createStreamBuilder(&builder);
    if (res != AAUDIO_OK || builder == nullptr) {
        LOG_ERROR("AaudioFifoPlayer: failed to create AAudio stream builder: "
                  << AAudio_convertResultToText(res));
        return false;
    }
    configure_builder(builder);
    AAudioStream* opened = nullptr;
    res = AAudioStreamBuilder_openStream(builder, &opened);
    AAudioStreamBuilder_delete(builder);
    if (res != AAUDIO_OK) {
        LOG_WARN("AaudioFifoPlayer: failed to recreate AAudio stream: "
                 << AAudio_convertResultToText(res));
        stream_ = nullptr;
        return false;
    }
    stream_ = opened;
    AAudioStream_requestStart(opened);
    LOG_INFO("AaudioFifoPlayer: AAudio stream recreated ("
             << AAudioStream_getSampleRate(static_cast<AAudioStream*>(stream_))
             << " Hz, " << AAudioStream_getChannelCount(static_cast<AAudioStream*>(stream_))
             << " ch)");
    return true;
}

bool AaudioFifoPlayer::create_fifo() {
    // Re-create the pipe cleanly.
    ::unlink(fifo_path().c_str());
    if (::mkfifo(fifo_path().c_str(), 0666) != 0 && errno != EEXIST) {
        LOG_ERROR("AaudioFifoPlayer: mkfifo(" << fifo_path() << ") failed: " << std::strerror(errno));
        return false;
    }
    ::chmod(fifo_path().c_str(), 0666);

    // CRITICAL: open O_RDWR so Linux never sends EOF when the writer
    // (the client playback thread) pauses or disconnects; the pump thread
    // keeps reading continuously instead of exiting. O_NONBLOCK additionally
    // lets close() unblock a writer/reader stuck on a full/empty pipe.
    fifo_fd_ = ::open(fifo_path().c_str(), O_RDWR | O_NONBLOCK);
    if (fifo_fd_ < 0) {
        LOG_ERROR("AaudioFifoPlayer: failed to open FIFO " << fifo_path() << ": " << std::strerror(errno));
        ::unlink(fifo_path().c_str());
        return false;
    }

    // Expand pipe capacity to 1 MB (~5 s of audio) so bursty Wi-Fi delivery
    // is buffered instead of stalling the network path.
    (void)::fcntl(fifo_fd_, F_SETPIPE_SZ, kFifoSizeBytes);
    return true;
}

void AaudioFifoPlayer::destroy_fifo() {
    if (fifo_fd_ >= 0) {
        ::close(fifo_fd_);
        fifo_fd_ = -1;
    }
    ::unlink(fifo_path().c_str());
}

void AaudioFifoPlayer::pump_loop() {
    const size_t bytes_per_frame = config_.bytes_per_frame();
    if (bytes_per_frame == 0) return;
    // ~20 ms chunk, like the standalone stream_daemon (960 frames @ 48 kHz).
    const size_t chunk_frames = (config_.sample_rate / 50) > 0 ? (config_.sample_rate / 50) : 1;
    std::vector<uint8_t> buffer(chunk_frames * bytes_per_frame);
    size_t residual = 0;

    // g_child_stop lets the forked helper's SIGTERM handler end the pump
    // gracefully; it is never set in the in-process path.
    while (!stop_pump_.load() && !g_child_stop.load()) {
        // Continuous blocking read (never returns 0 / EOF thanks to O_RDWR).
        // poll() bounds the wait so shutdown stays responsive.
        struct pollfd pfd = {fifo_fd_, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, kFifoPollMs);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;  // EBADF: fifo closed (shutdown)
        }
        if (pr == 0 || !(pfd.revents & POLLIN)) continue;

        ssize_t bytes_read = ::read(fifo_fd_, buffer.data() + residual, buffer.size() - residual);
        if (bytes_read <= 0) continue;  // EAGAIN or interrupted; never EOF

        const size_t total_bytes = residual + static_cast<size_t>(bytes_read);
        const size_t frames_available = total_bytes / bytes_per_frame;
        residual = total_bytes % bytes_per_frame;

        if (frames_available > 0) {
            // Resolve the stream under the lock, but DO NOT hold the lock
            // across AAudioStream_write(): close() (shutdown) must be able to
            // close the stream to unblock a write that is stuck on a dead
            // output path (the legacy AudioTrack path can block forever).
            AAudioStream* stream = nullptr;
            {
                std::lock_guard<std::mutex> lock(stream_mutex_);
                if (stream_ == nullptr) break;  // closed (shutdown)
                stream = static_cast<AAudioStream*>(stream_);
                if (!ensure_stream_started_locked()) continue;
            }

            const aaudio_result_t frames_written =
                AAudioStream_write(stream, buffer.data(), static_cast<int32_t>(frames_available),
                                   kWriteTimeoutNs);

            if (frames_written < 0 || static_cast<size_t>(frames_written) < frames_available) {
                // The write failed or only partially completed (timeout): the
                // stream may be paused or wedged. Auto-restart it; if it stays
                // wedged, recreate the session. Log at most once every 5 s so
                // a wedged stream can't flood the log.
                ++consecutive_write_failures_;
                const uint64_t now_ms = get_time_ms();
                if (now_ms - last_write_warn_ms_ >= 5000) {
                    last_write_warn_ms_ = now_ms;
                    LOG_WARN("AaudioFifoPlayer: AAudioStream_write wrote " << frames_written
                             << " of " << frames_available << " frames (state="
                             << AAudioStream_getState(stream) << ", written="
                             << AAudioStream_getFramesWritten(stream) << ", read="
                             << AAudioStream_getFramesRead(stream) << ", failures="
                             << consecutive_write_failures_ << ')');
                }
                const bool disconnected =
                    frames_written == AAUDIO_ERROR_DISCONNECTED ||
                    AAudioStream_getState(stream) == AAUDIO_STREAM_STATE_DISCONNECTED;
                std::lock_guard<std::mutex> lock(stream_mutex_);
                if (stream_ == nullptr) break;  // closed concurrently (shutdown)
                if (disconnected) {
                    // Routing changed (headphones unplugged, BT reconnect, ...).
                    if (now_ms - last_rebuild_ms_ >= kMinRebuildIntervalMs) {
                        last_rebuild_ms_ = now_ms;
                        LOG_WARN("AaudioFifoPlayer: stream disconnected; recreating AAudio stream");
                        rebuild_stream_locked();
                    }
                } else if (consecutive_write_failures_ >= kStallRebuildThreshold) {
                    // Persistent stall: the session opened but never renders
                    // (the data path is dead). Recreate the session; after the
                    // first stall-rebuild, drop to the deep-buffer performance
                    // mode (legacy AudioTrack path instead of MMAP).
                    if (now_ms - last_rebuild_ms_ >= kMinRebuildIntervalMs) {
                        last_rebuild_ms_ = now_ms;
                        ++stall_rebuilds_;
                        if (stall_rebuilds_ > kMaxStallRebuilds) {
                            // Never going to render on this device; stop the
                            // pointless rebuild loop and mark the player dead
                            // so the playback thread stops feeding it.
                            LOG_ERROR("AaudioFifoPlayer: giving up on AAudio after "
                                      << kMaxStallRebuilds << " stalled stream rebuilds - the "
                                      "AAudio data path cannot render on this device. Restart "
                                      "the client with '-d agm' or '-d default' (or reboot "
                                      "Android audio: 'stop audioserver && start audioserver').");
                            if (stream_ != nullptr) {
                                AAudioStream_close(static_cast<AAudioStream*>(stream_));
                                stream_ = nullptr;
                            }
                            frames_in_flight_.store(0);
                            break;  // exits the while loop; lock released by scope exit
                        }
                        if (stall_rebuilds_ > 1 && !deep_retry_) {
                            deep_retry_ = true;
                            LOG_WARN("AaudioFifoPlayer: stream keeps stalling; retrying with "
                                     "deep-buffer performance mode");
                        }
                        LOG_WARN("AaudioFifoPlayer: stream wedged after "
                                 << consecutive_write_failures_
                                 << " failed writes; recreating AAudio stream");
                        rebuild_stream_locked();
                    }
                } else {
                    AAudioStream_requestStart(stream);
                }
                continue;
            }

            consecutive_write_failures_ = 0;
            {
                std::lock_guard<std::mutex> lock(stream_mutex_);
                if (stream_ == nullptr) break;  // closed concurrently (shutdown)
                const int64_t written_total = AAudioStream_getFramesWritten(stream);
                const int64_t read_total = AAudioStream_getFramesRead(stream);
                frames_in_flight_.store(written_total - read_total);
            }
        }

        // Keep unused fractional-frame bytes for the next iteration.
        if (residual > 0) {
            std::memmove(buffer.data(), buffer.data() + (frames_available * bytes_per_frame), residual);
        }
    }

    // The pump can exit with the player still "open" only when it gave up on
    // a permanently stalled stream (the give-up path closed the stream and
    // broke out of the loop). Mark the player closed so write_frames() stops
    // feeding a dead stream.
    if (stream_ == nullptr && is_open_.load()) {
        is_open_.store(false);
        if (fifo_fd_ >= 0) {
            ::close(fifo_fd_);
            fifo_fd_ = -1;
        }
        ::unlink(fifo_path().c_str());
    }
}

#endif  // __ANDROID__ && AAUDIO_ENABLED

} // namespace audiorouter
