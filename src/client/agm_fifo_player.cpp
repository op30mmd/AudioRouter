#include "agm_fifo_player.hpp"
#include "android_helpers.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>

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

    const uint32_t rate = config.sample_rate > 0 ? static_cast<uint32_t>(config.sample_rate) : 48000;

    if (::mkfifo(kFifoPath, 0666) != 0) {
        if (errno != EEXIST) {
            LOG_ERROR("AgmFifoPlayer: mkfifo(" << kFifoPath << ") failed: " << std::strerror(errno));
            return false;
        }
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

    agmplay_pid_ = pid;
    LOG_INFO("AgmFifoPlayer: spawned " << kAgmplayPath << " (pid " << pid << ", backend '"
             << backend_ << "', " << rate << " Hz mono S16)" << ", waiting for it to open "
             << kFifoPath << "...");

    if (!wait_for_reader(kFifoReaderTimeoutMs)) {
        LOG_ERROR("AgmFifoPlayer: agmplay did not open the FIFO within "
                  << kFifoReaderTimeoutMs << " ms (is /vendor/bin/agmplay present?)");
        close();
        return false;
    }

    write_wav_header(rate);

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
        LOG_DEBUG("AgmFifoPlayer: FIFO write failed: " << (n < 0 ? std::strerror(errno) : "EOF"));
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
