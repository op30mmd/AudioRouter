#include "aaudio_player.hpp"
#include "android_helpers.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#if defined(__ANDROID__) && defined(AAUDIO_ENABLED)
#include <aaudio/AAudio.h>
#endif

namespace {

// Pipe capacity for the PCM FIFO: 64 KB ~= 341 ms of 48 kHz stereo S16
// (192 KB/s). The pipe is a DECOUPLING buffer between the playback thread and
// the AAudio pump, not a network burst absorber - that is the jitter buffer's
// job (up to ~1.28 s). A larger pipe (the stream_daemon's 1 MB = ~5.3 s) lets
// a stalled stream bury seconds of stale audio that then plays back before
// live audio, which the user hears as a multi-second delay after any hiccup.
// 64 KB holds ~17 pump chunks of slack while keeping the stale-audio bound at
// ~341 ms (and the pump drains it to near-zero in steady state anyway).
constexpr int kFifoSizeBytes = 65536;

// How long AAudioStream_write() may block for space in the AAudio buffer.
// Matches the standalone stream_daemon (500 ms), which is proven to work on
// real devices: shorter timeouts combined with rebuild logic killed streams
// that were merely slow to start consuming.
constexpr int64_t kWriteTimeoutNs = 500000000LL;

// Poll timeouts keep shutdown responsive: the pump polls the FIFO for 100 ms
// at a time and write_frames() waits at most 50 ms for pipe space, both
// re-checking the open/stop flags each iteration.
constexpr int kFifoPollMs = 100;
constexpr int kWritePollMs = 50;

// Minimum gap between AAudio stream recreations (a flapping routing change
// must not thrash stream open/close).
constexpr uint64_t kMinRebuildIntervalMs = 1000;

// The watchdog considers a stream wedged after this many consecutive failed
// writes. At the 500 ms write timeout this is ~10 s - far longer than any
// healthy stream needs to start consuming (the stream_daemon never gives up
// at all), but short enough that a truly dead HAL session is recovered by
// recreating the stream, and eventually by falling back to AGM/ALSA.
constexpr int kStallRebuildThreshold = 20;

// Give up (and let the client fall back to AGM/ALSA) after this many stalled
// stream recreations.
constexpr int kMaxStallRebuilds = 3;

} // namespace

namespace audiorouter {

AaudioFifoPlayer::AaudioFifoPlayer() = default;

AaudioFifoPlayer::~AaudioFifoPlayer() {
    close();
}

std::string AaudioFifoPlayer::fifo_path() {
    // The stream_daemon uses /data/local/tmp/audio_pipe as root and it works,
    // so root uses /data/local/tmp here too (no HOME guessing - under `su`
    // from Termux, Magisk sets HOME=/ which is read-only). Non-root uses the
    // app user's own writable home.
    static const std::string path = [] {
        if (getuid() == 0) {
            return std::string("/data/local/tmp/audiorouter_aaudio.fifo");
        }
        if (const char* home = std::getenv("HOME");
            home != nullptr && home[0] != '\0' && std::strcmp(home, "/") != 0) {
            return std::string(home) + "/audiorouter_aaudio.fifo";
        }
        uid_t uid = 0;
        gid_t gid = 0;
        std::string termux_home;
        if (AndroidHelpers::termux_user(&uid, &gid, &termux_home) && getuid() == uid) {
            return termux_home + "/audiorouter_aaudio.fifo";
        }
        return std::string("/data/local/tmp/audiorouter_aaudio.fifo");
    }();
    return path;
}

bool AaudioFifoPlayer::open(const AudioConfig& config, const std::string& device_name) {
#if defined(__ANDROID__) && defined(AAUDIO_ENABLED)
    if (is_open_.load()) close();

    // Fresh session: reset the watchdog state from any previous run.
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

    if (!create_fifo()) return false;

    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        AAudioStreamBuilder* builder = nullptr;
        aaudio_result_t res = AAudio_createStreamBuilder(&builder);
        if (res != AAUDIO_OK || builder == nullptr) {
            LOG_ERROR("AaudioFifoPlayer: failed to create AAudio stream builder: "
                      << AAudio_convertResultToText(res));
            destroy_fifo();
            return false;
        }
        configure_builder(builder);
        AAudioStream* opened = nullptr;
        res = AAudioStreamBuilder_openStream(builder, &opened);
        AAudioStreamBuilder_delete(builder);
        if (res != AAUDIO_OK) {
            LOG_ERROR("AaudioFifoPlayer: failed to open AAudio stream: "
                      << AAudio_convertResultToText(res));
            destroy_fifo();
            return false;
        }
        stream_ = opened;
        AAudioStream_requestStart(opened);
    }

    // No readiness probe here - deliberately. The stream_daemon (identical
    // engine) never waits or probes and works: a stream that is slow to reach
    // STARTED or to start consuming is given all the time it needs, and the
    // pump below keeps writing regardless. A genuinely dead stream is caught
    // later by the pump's lenient watchdog, which recreates the stream and
    // eventually gives up so the client can fall back to AGM/ALSA.
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
#else
    (void)config;
    (void)device_name;
    LOG_INFO("AaudioFifoPlayer: AAudio not available on this platform (requires "
             "an Android API 26+ build with libaaudio)");
    return false;
#endif
}

void AaudioFifoPlayer::close() {
#if defined(__ANDROID__) && defined(AAUDIO_ENABLED)
    stop_pump_.store(true);
    // Close the AAudio stream FIRST, before joining the pump: a write stuck
    // on a dead output path is unblocked by closing the stream, otherwise the
    // join below would hang shutdown.
    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        if (stream_ != nullptr) {
            AAudioStream_close(static_cast<AAudioStream*>(stream_));
            stream_ = nullptr;
        }
    }
    if (pump_thread_.joinable()) pump_thread_.join();

    if (fifo_fd_ >= 0) {
        ::close(fifo_fd_);
        fifo_fd_ = -1;
    }
    ::unlink((resolved_fifo_.empty() ? fifo_path() : resolved_fifo_).c_str());

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
    int64_t in_flight = frames_in_flight_.load();
#if defined(__ANDROID__) && defined(AAUDIO_ENABLED)
    if (stream_mutex_.try_lock()) {
        if (stream_ != nullptr) {
            auto* stream = static_cast<AAudioStream*>(stream_);
            in_flight = AAudioStream_getFramesWritten(stream) - AAudioStream_getFramesRead(stream);
        }
        stream_mutex_.unlock();
    }
#endif
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

    // NOTE: usage/content-type are intentionally NOT set. The standalone
    // stream_daemon (identical engine, proven on this device) sets none of
    // them; usage hints can steer the HAL onto a routing path that never
    // starts consuming, and the daemon works without them.
    AAudioStreamBuilder_setPerformanceMode(builder,
        (deep_retry_ || mode_ == "deep") ? AAUDIO_PERFORMANCE_MODE_NONE
                                         : AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
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
    resolved_fifo_ = fifo_path();
    // Re-create the pipe cleanly.
    ::unlink(resolved_fifo_.c_str());
    if (::mkfifo(resolved_fifo_.c_str(), 0666) != 0 && errno != EEXIST) {
        LOG_ERROR("AaudioFifoPlayer: mkfifo(" << resolved_fifo_ << ") failed: " << std::strerror(errno));
        return false;
    }
    ::chmod(resolved_fifo_.c_str(), 0666);

    // CRITICAL: open O_RDWR so Linux never sends EOF when the writer
    // (the client playback thread) pauses or disconnects; the pump thread
    // keeps reading continuously instead of exiting. O_NONBLOCK additionally
    // lets close() unblock a writer/reader stuck on a full/empty pipe.
    fifo_fd_ = ::open(resolved_fifo_.c_str(), O_RDWR | O_NONBLOCK);
    if (fifo_fd_ < 0) {
        LOG_ERROR("AaudioFifoPlayer: failed to open FIFO " << resolved_fifo_ << ": " << std::strerror(errno));
        ::unlink(resolved_fifo_.c_str());
        return false;
    }

    // Expand pipe capacity to 1 MB (~5 s of audio) so bursty Wi-Fi delivery
    // is buffered instead of stalling the network path.
    (void)::fcntl(fifo_fd_, F_SETPIPE_SZ, kFifoSizeBytes);
    return true;
}

void AaudioFifoPlayer::drain_fifo() {
    if (fifo_fd_ < 0) return;
    // The pipe is O_NONBLOCK, so read() returns EAGAIN once it is empty.
    // 64 iterations of 4 KB covers the whole 64 KB pipe (and then some).
    char tmp[4096];
    for (int i = 0; i < 64; ++i) {
        const ssize_t n = ::read(fifo_fd_, tmp, sizeof(tmp));
        if (n <= 0) break;  // EAGAIN (drained) or error
    }
}

void AaudioFifoPlayer::destroy_fifo() {
    if (fifo_fd_ >= 0) {
        ::close(fifo_fd_);
        fifo_fd_ = -1;
    }
    ::unlink((resolved_fifo_.empty() ? fifo_path() : resolved_fifo_).c_str());
}

void AaudioFifoPlayer::pump_loop() {
    const size_t bytes_per_frame = config_.bytes_per_frame();
    if (bytes_per_frame == 0) return;
    // ~20 ms chunk, like the standalone stream_daemon (960 frames @ 48 kHz).
    const size_t chunk_frames = (config_.sample_rate / 50) > 0 ? (config_.sample_rate / 50) : 1;
    std::vector<uint8_t> buffer(chunk_frames * bytes_per_frame);
    size_t residual = 0;

    while (!stop_pump_.load()) {
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
            // output path.
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
                // stream may be paused or still ramping up, or wedged. Auto-
                // restart it; the lenient watchdog recreates it after a long
                // stall, then gives up so the client falls back to AGM/ALSA.
                // Log at most once every 5 s so a wedged stream can't flood
                // the log.
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
                        // Drop whatever accumulated while the old stream was
                        // dead so recovery starts at live audio, not stale.
                        drain_fifo();
                    }
                } else if (consecutive_write_failures_ >= kStallRebuildThreshold) {
                    // Persistent stall: the session opened but never renders.
                    // Recreate the session; on the second stall-rebuild, drop
                    // to the deep-buffer performance mode (which starts on
                    // HALs where the low-latency session never renders).
                    if (now_ms - last_rebuild_ms_ >= kMinRebuildIntervalMs) {
                        last_rebuild_ms_ = now_ms;
                        ++stall_rebuilds_;
                        if (stall_rebuilds_ > 1 && !deep_retry_) {
                            deep_retry_ = true;
                            LOG_WARN("AaudioFifoPlayer: stream keeps stalling; retrying with "
                                     "deep-buffer performance mode");
                        }
                        if (stall_rebuilds_ > kMaxStallRebuilds) {
                            // Never going to render on this device; stop the
                            // rebuild loop and mark the player dead so the
                            // client falls back to AGM/ALSA.
                            LOG_ERROR("AaudioFifoPlayer: giving up on AAudio after "
                                      << kMaxStallRebuilds << " stalled stream rebuilds - the "
                                      "AAudio output does not render on this device. The "
                                      "client will fall back to AGM/ALSA (use '-d agm' "
                                      "directly for the best experience on this device).");
                            if (stream_ != nullptr) {
                                AAudioStream_close(static_cast<AAudioStream*>(stream_));
                                stream_ = nullptr;
                            }
                            frames_in_flight_.store(0);
                            is_open_.store(false);
                            break;  // exits the while loop; lock released by scope exit
                        }
                        LOG_WARN("AaudioFifoPlayer: stream wedged after "
                                 << consecutive_write_failures_
                                 << " failed writes; recreating AAudio stream");
                        rebuild_stream_locked();
                        // Drop stale buffered audio so the fresh stream
                        // starts at live audio instead of replaying the
                        // stall period.
                        drain_fifo();
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
}

#endif  // __ANDROID__ && AAUDIO_ENABLED

} // namespace audiorouter
