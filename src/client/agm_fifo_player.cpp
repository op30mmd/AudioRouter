#include "agm_fifo_player.hpp"
#include "android_helpers.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// agmplay's RIFF chunk IDs (agmplay.c: ID_RIFF/ID_FMT/ID_DATA).
constexpr uint32_t kIdRiff = 0x46464952;
constexpr uint32_t kIdWave = 0x45564157;
constexpr uint32_t kIdFmt = 0x20746d66;
constexpr uint32_t kIdData = 0x61746164;

// FIFO used to stream PCM into the agmplay subprocess.
constexpr const char* kFifoPath = "/data/local/tmp/audiorouter_agm.fifo";
constexpr unsigned int kAgmCard = 100;
constexpr unsigned int kAgmDevice = 100;

// How long to wait for agmplay to open the FIFO before giving up.
constexpr int kFifoReaderTimeoutMs = 10000;

// Candidate binary locations for agmplay (vendor to system fallback)
constexpr const char* kAgmplayCandidates[] = {
    "/vendor/bin/agmplay",
    "/system/bin/agmplay",
    "/vendor/bin/hw/vendor.qti.media.c2audio@1.0-service", // not agmplay but placeholder for search logic
};

std::string resolve_agmplay_path() {
    for (const char* cand : {"/vendor/bin/agmplay", "/system/bin/agmplay"}) {
        if (::access(cand, X_OK) == 0) {
            return cand;
        }
    }
    // Check existence even if not executable (some devices have different perms)
    for (const char* cand : {"/vendor/bin/agmplay", "/system/bin/agmplay"}) {
        if (::access(cand, F_OK) == 0) {
            return cand;
        }
    }
    return "/vendor/bin/agmplay"; // default, will be logged if spawn fails
}

} // namespace

namespace audiorouter {

AgmFifoPlayer::AgmFifoPlayer() : backend_("CODEC_DMA-LPAIF_RXTX-RX-1") {}

AgmFifoPlayer::~AgmFifoPlayer() {
    close();
}

bool AgmFifoPlayer::open(const AudioConfig& config, const std::string& device_name) {
#if defined(__linux__) || defined(__ANDROID__)
    if (is_open_) close();

    // Ignore SIGPIPE - writing to FIFO after agmplay exits would otherwise kill the client
    // with SIGPIPE (default action: terminate). This matches the "dies in middle" bug
    // observed on Bengal where agmplay backend exits after opening FIFO.
    ::signal(SIGPIPE, SIG_IGN);

    config_ = config;
    backend_ = "CODEC_DMA-LPAIF_RXTX-RX-1";
    const std::string name = device_name.empty() ? "agm" : device_name;
    if (name.rfind("agm:", 0) == 0) {
        std::string rest = name.substr(4);
        if (!rest.empty()) backend_ = rest;
    }

    // Route the codec to the speaker before agmplay starts (best effort).
    AndroidHelpers::apply_speaker_routing();

    const uint32_t rate = config.sample_rate > 0 ? static_cast<uint32_t>(config.sample_rate) : 48000;

    if (::mkfifo(kFifoPath, 0666) != 0) {
        if (errno != EEXIST) {
            LOG_ERROR("AgmFifoPlayer: mkfifo(" << kFifoPath << ") failed: " << std::strerror(errno));
            return false;
        }
    }

    std::string agmplay_path = resolve_agmplay_path();
    if (::access(agmplay_path.c_str(), F_OK) != 0) {
        LOG_ERROR("AgmFifoPlayer: agmplay binary not found at any known path (tried /vendor/bin/agmplay, /system/bin/agmplay). AGM backend unavailable on this device.");
        ::unlink(kFifoPath);
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        LOG_ERROR("AgmFifoPlayer: fork() failed: " << std::strerror(errno));
        ::unlink(kFifoPath);
        return false;
    }

    if (pid == 0) {
        // Child: exec agmplay. Only async-signal-safe calls allowed.
        // IMPORTANT: Clear Termux's LD_PRELOAD (libtermux-exec.so) which blocks vendor
        // binaries from loading in the default linker namespace.
        // We use execve with a minimal clean environment to avoid:
        //   CANNOT LINK EXECUTABLE "agmplay": library "/data/data/com.termux/files/usr/lib/libtermux-exec.so" needed...
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        // Clean environment: vendor lib paths + PATH, no LD_PRELOAD
        // Keep LD_LIBRARY_PATH broad to satisfy both QCOM and generic dependencies
        const char* envp[] = {
            "LD_LIBRARY_PATH=/vendor/lib64:/vendor/lib:/system/lib64:/system/lib",
            "PATH=/vendor/bin:/system/bin:/system/xbin",
            "ANDROID_ROOT=/system",
            "ANDROID_DATA=/data",
            nullptr
        };
        const char* args[] = {
            agmplay_path.c_str(),
            kFifoPath,
            "-D", "100",
            "-d", "100",
            "-i", backend_.c_str(),
            nullptr
        };
        ::execve(agmplay_path.c_str(), const_cast<char* const*>(args), const_cast<char* const*>(envp));
        // If execve fails (e.g., agmplay not at resolved path), try fallback via execv with PATH search
        // This second attempt uses clean env via environ manipulation, but we keep it simple: exit.
        _exit(127);  // exec failed
    }

    agmplay_pid_ = pid;
    LOG_INFO("AgmFifoPlayer: spawned " << agmplay_path << " (pid " << pid << ", backend '"
             << backend_ << "', " << rate << " Hz mono S16)" << ", waiting for it to open "
             << kFifoPath << "...");

    if (!wait_for_reader(kFifoReaderTimeoutMs)) {
        LOG_ERROR("AgmFifoPlayer: agmplay did not open the FIFO within "
                  << kFifoReaderTimeoutMs << " ms. Possible reasons: "
                  << "binary missing, wrong backend '" << backend_ << "' for this device, "
                  << "or linker namespace blocking. Try running with clean env: "
                  << "env -i LD_LIBRARY_PATH=/vendor/lib64:/vendor/lib /vendor/bin/agmplay --help");
        close();
        return false;
    }

    write_wav_header(rate);

    // Critical: On Bengal, many backends (RX-0, RX-1, etc.) cause agmplay to exit
    // immediately after opening FIFO (invalid graph). If that happens, we should
    // fail open() quickly so supervisor can fallback to direct PCM nodes (which
    // were working before our AGM default change). Check if process still alive
    // 300ms after header write - if died, treat as open failure.
    sleep_ms(300);
    if (agmplay_pid_ > 0) {
        int status = 0;
        pid_t result = ::waitpid(agmplay_pid_, &status, WNOHANG);
        if (result == agmplay_pid_) {
            int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            int sig = WIFSIGNALED(status) ? WTERMSIG(status) : -1;
            LOG_WARN("AgmFifoPlayer: agmplay died immediately after WAV header (exit_code=" << exit_code << " signal=" << sig << " backend='" << backend_ << "'). Backend invalid for this device - will fallback to direct PCM");
            close();
            return false;
        }
    }

    is_open_ = true;
    LOG_INFO("AgmFifoPlayer: streaming into agmplay (pid " << agmplay_pid_ << ", backend '"
             << backend_ << "')");
    return true;
#else
    (void)config;
    (void)device_name;
    LOG_INFO("AgmFifoPlayer: AGM playback not available on this platform");
    return false;
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

    // Switch to blocking writes once the reader is present.
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
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

    const ssize_t n = ::write(fifo_fd_, &hdr, sizeof(hdr));
    if (n != static_cast<ssize_t>(sizeof(hdr))) {
        LOG_ERROR("AgmFifoPlayer: failed to write WAV header to FIFO: "
                  << (n < 0 ? std::strerror(errno) : "short write"));
    }
}

void AgmFifoPlayer::close() {
#if defined(__linux__) || defined(__ANDROID__)
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
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!is_open_ || fifo_fd_ < 0) return 0;

    size_t written = 0;
    while (written < bytes) {
        const ssize_t n = ::write(fifo_fd_, reinterpret_cast<const uint8_t*>(src) + written,
                                  bytes - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        // EPIPE/EOF: agmplay exited (or never started consuming).
        // Check if agmplay subprocess is still alive
        bool agm_alive = true;
        if (agmplay_pid_ > 0) {
            int status = 0;
            pid_t result = ::waitpid(agmplay_pid_, &status, WNOHANG);
            if (result == agmplay_pid_) {
                agm_alive = false;
                int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                int sig = WIFSIGNALED(status) ? WTERMSIG(status) : -1;
                LOG_WARN("AgmFifoPlayer: agmplay subprocess (pid " << agmplay_pid_ << ") died - exit_code=" << exit_code << " signal=" << sig << " backend='" << backend_ << "' may be wrong for this device. Try agm:CODEC_DMA-LPAIF_RXTX-RX-0, RX-1, RX-2");
                // Mark as not open to trigger fallback logic, but don't close yet - let supervisor handle
                // For now, keep is_open_ true but return 0 so playback thread sleeps and retries
                agmplay_pid_ = -1;
            }
        }
        if (!agm_alive) {
            LOG_WARN("AgmFifoPlayer: FIFO write failed because agmplay died (EPIPE). Returning 0, client will keep trying / fallback may kick in");
        } else {
            LOG_WARN("AgmFifoPlayer: FIFO write failed: " << (n < 0 ? std::strerror(errno) : "EOF") << " (agmplay may still be alive but not consuming)");
        }
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
