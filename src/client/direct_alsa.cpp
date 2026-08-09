#include "direct_alsa.hpp"
#include "../common/logger.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstring>
#include <dirent.h>

#if defined(__linux__) || defined(__ANDROID__)
    #include <sound/asound.h>
#endif

namespace audiorouter {

DirectAlsaPlayer::DirectAlsaPlayer()
    : fd_(-1), period_size_frames_(240), buffer_size_frames_(960), is_open_(false) {}

DirectAlsaPlayer::~DirectAlsaPlayer() {
    close();
}

std::vector<std::string> DirectAlsaPlayer::enumerate_kernel_pcm_devices() {
    std::vector<std::string> devices;
#if defined(__linux__) || defined(__ANDROID__)
    DIR* dir = opendir("/dev/snd");
    if (!dir) return devices;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        // Looking for playback devices like pcmC0D0p, pcmC0D1p, etc.
        if (name.rfind("pcmC", 0) == 0 && name.back() == 'p') {
            devices.push_back("/dev/snd/" + name);
        }
    }
    closedir(dir);
#endif
    return devices;
}

bool DirectAlsaPlayer::open(const AudioConfig& config, const std::string& device_name) {
    if (is_open_) {
        close();
    }

    config_ = config;
    device_path_ = device_name.empty() ? "/dev/snd/pcmC0D0p" : device_name;

#if defined(__linux__) || defined(__ANDROID__)
    // Build a prioritized list of fallback candidates
    std::vector<std::string> candidates;
    std::string primary_path = device_name.empty() ? "/dev/snd/pcmC0D0p" : device_name;
    candidates.push_back(primary_path);
    if (primary_path != "/dev/snd/pcmC0D0p") {
        candidates.push_back("/dev/snd/pcmC0D0p");
    }

    std::vector<std::string> all_devices = enumerate_kernel_pcm_devices();
    for (const auto& dev : all_devices) {
        bool already_added = false;
        for (const auto& c : candidates) {
            if (c == dev) {
                already_added = true;
                break;
            }
        }
        if (!already_added) {
            candidates.push_back(dev);
        }
    }

    bool success = false;
    for (const auto& candidate : candidates) {
        fd_ = ::open(candidate.c_str(), O_RDWR);
        if (fd_ < 0) {
            continue;
        }

        // Set hardware parameters via ioctl
        struct snd_pcm_hw_params params;
        std::memset(&params, 0, sizeof(params));

        // Initialize hw params mask/intervals to any
        for (int i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; ++i) {
            params.intervals[i - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = 0;
            params.intervals[i - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = UINT32_MAX;
        }
        for (int i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_MASK; ++i) {
            params.masks[i - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits[0] = 0xFFFFFFFF;
            params.masks[i - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits[1] = 0xFFFFFFFF;
        }

        // Access: RW interleaved
        params.masks[SNDRV_PCM_HW_PARAM_ACCESS - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits[0] = (1 << SNDRV_PCM_ACCESS_RW_INTERLEAVED);

        // Format: S16_LE
        params.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits[0] = (1 << SNDRV_PCM_FORMAT_S16_LE);

        // Channels
        params.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = config_.channels;
        params.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = config_.channels;

        // Rate
        params.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = config_.sample_rate;
        params.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = config_.sample_rate;

        // Period size & buffer size
        period_size_frames_ = config_.frames_per_packet > 0 ? config_.frames_per_packet : 240;
        buffer_size_frames_ = period_size_frames_ * 4;

        params.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = static_cast<unsigned int>(period_size_frames_);
        params.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = static_cast<unsigned int>(period_size_frames_ * 2);
        params.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = static_cast<unsigned int>(buffer_size_frames_);
        params.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = static_cast<unsigned int>(buffer_size_frames_ * 4);

        if (ioctl(fd_, SNDRV_PCM_IOCTL_HW_PARAMS, &params) < 0) {
            LOG_DEBUG("DirectAlsaPlayer: SNDRV_PCM_IOCTL_HW_PARAMS failed for candidate " << candidate << ": " << strerror(errno));
            ::close(fd_);
            fd_ = -1;
            continue;
        }

        // Prepare PCM
        if (ioctl(fd_, SNDRV_PCM_IOCTL_PREPARE) < 0) {
            LOG_DEBUG("DirectAlsaPlayer: SNDRV_PCM_IOCTL_PREPARE failed for candidate " << candidate << ": " << strerror(errno));
            ::close(fd_);
            fd_ = -1;
            continue;
        }

        device_path_ = candidate;
        success = true;
        break;
    }

    if (!success) {
        LOG_ERROR("DirectAlsaPlayer: Failed to open and configure any fallback kernel PCM devices in /dev/snd/ (Last error: " << strerror(errno) << ")");
        return false;
    }

    is_open_ = true;
    LOG_INFO("DirectAlsaPlayer: Successfully opened and configured kernel ALSA device '" << device_path_ << "' (" << config_.to_string() << ")");
    return true;
#else
    LOG_INFO("DirectAlsaPlayer: Mock open on non-Linux platform");
    is_open_ = true;
    return true;
#endif
}

void DirectAlsaPlayer::close() {
    if (is_open_) {
#if defined(__linux__) || defined(__ANDROID__)
        if (fd_ >= 0) {
            ioctl(fd_, SNDRV_PCM_IOCTL_DROP);
            ::close(fd_);
            fd_ = -1;
        }
#endif
        is_open_ = false;
        LOG_INFO("DirectAlsaPlayer: Device closed");
    }
}

bool DirectAlsaPlayer::is_open() const {
    return is_open_;
}

size_t DirectAlsaPlayer::write_frames(const void* pcm_data, size_t num_frames) {
    if (!is_open_ || !pcm_data || num_frames == 0) return 0;

#if defined(__linux__) || defined(__ANDROID__)
    if (fd_ < 0) return 0;

    struct snd_xferi xferi;
    xferi.result = 0;
    xferi.buf = const_cast<void*>(pcm_data);
    xferi.frames = num_frames;

    int ret = ioctl(fd_, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xferi);
    if (ret < 0) {
        if (errno == EPIPE) {
            // Underrun - prepare device again
            LOG_DEBUG("DirectAlsaPlayer: Kernel buffer underrun (XRUN), recovering...");
            ioctl(fd_, SNDRV_PCM_IOCTL_PREPARE);
            ret = ioctl(fd_, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xferi);
        } else if (errno == ESTRPIPE) {
            // Suspended
            ioctl(fd_, SNDRV_PCM_IOCTL_RESUME);
            ioctl(fd_, SNDRV_PCM_IOCTL_PREPARE);
        }
    }

    if (ret >= 0) {
        return xferi.result > 0 ? xferi.result : num_frames;
    }
    return 0;
#else
    return num_frames;
#endif
}

size_t DirectAlsaPlayer::get_buffer_delay_frames() const {
    if (!is_open_) return 0;
#if defined(__linux__) || defined(__ANDROID__)
    if (fd_ < 0) return 0;
    snd_pcm_sframes_t delay = 0;
    if (ioctl(fd_, SNDRV_PCM_IOCTL_DELAY, &delay) >= 0) {
        return delay > 0 ? static_cast<size_t>(delay) : 0;
    }
#endif
    return 0;
}

void DirectAlsaPlayer::flush() {
    if (!is_open_) return;
#if defined(__linux__) || defined(__ANDROID__)
    if (fd_ >= 0) {
        ioctl(fd_, SNDRV_PCM_IOCTL_DROP);
        ioctl(fd_, SNDRV_PCM_IOCTL_PREPARE);
    }
#endif
}

std::string DirectAlsaPlayer::get_device_name() const {
    return device_path_;
}

} // namespace audiorouter
