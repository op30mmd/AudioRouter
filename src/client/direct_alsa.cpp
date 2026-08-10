#include "direct_alsa.hpp"
#include "../common/logger.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <cstring>
#include <dirent.h>
#include <algorithm>
#include <climits>

#if defined(__linux__) || defined(__ANDROID__)
    #include <sound/asound.h>

namespace {

// Fill an snd_pcm_hw_params struct with "any" masks/intervals, i.e. no
// constraints, exactly like alsa-lib's snd_pcm_hw_params_any().
void init_any_params(struct snd_pcm_hw_params& params) {
    std::memset(&params, 0, sizeof(params));

    for (int k = SNDRV_PCM_HW_PARAM_FIRST_MASK; k <= SNDRV_PCM_HW_PARAM_LAST_MASK; ++k) {
        auto& mask = params.masks[k - SNDRV_PCM_HW_PARAM_FIRST_MASK];
        for (size_t b = 0; b < sizeof(mask.bits) / sizeof(mask.bits[0]); ++b) {
            mask.bits[b] = 0xFFFFFFFFu;
        }
    }

    for (int k = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; k <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; ++k) {
        auto& iv = params.intervals[k - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        iv.min = 0;
        iv.max = UINT32_MAX;
        iv.openmin = 0;
        iv.openmax = 0;
        iv.integer = 0;
        iv.empty = 0;
    }
}

// Set a single hw param to an exact value.
void set_param_value(struct snd_pcm_hw_params& params, int param, unsigned int value) {
    if (param >= SNDRV_PCM_HW_PARAM_FIRST_MASK && param <= SNDRV_PCM_HW_PARAM_LAST_MASK) {
        auto& mask = params.masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
        for (size_t b = 0; b < sizeof(mask.bits) / sizeof(mask.bits[0]); ++b) {
            mask.bits[b] = 0;
        }
        mask.bits[value / 32] = (1u << (value % 32));
    } else if (param >= SNDRV_PCM_HW_PARAM_FIRST_INTERVAL && param <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL) {
        auto& iv = params.intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        iv.min = value;
        iv.max = value;
    }
}

// Re-open the primary candidate and ask the kernel (HW_REFINE with "any"
// params) what this device actually supports, so failures become actionable.
void log_device_capabilities(const std::vector<std::string>& candidates, const std::string& last_error) {
    if (candidates.empty()) return;

    const std::string& probe_path = candidates.front();
    int probe_fd = ::open(probe_path.c_str(), O_RDWR | O_NONBLOCK);
    if (probe_fd < 0) {
        LOG_DEBUG("DirectAlsaPlayer: Could not re-open '" << probe_path << "' for capability probe: " << strerror(errno));
        return;
    }

    struct snd_pcm_hw_params probe;
    init_any_params(probe);
    if (ioctl(probe_fd, SNDRV_PCM_IOCTL_HW_REFINE, &probe) == 0) {
        const auto& rate = probe.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        const auto& channels = probe.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        const auto& period = probe.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        const auto& buffer = probe.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        const auto& access = probe.masks[SNDRV_PCM_HW_PARAM_ACCESS - SNDRV_PCM_HW_PARAM_FIRST_MASK];
        const auto& formats = probe.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK];

        LOG_WARN("DirectAlsaPlayer: Device '" << probe_path << "' capabilities (probe failed with: " << last_error << "):");
        LOG_WARN("  rate [" << rate.min << ", " << rate.max << "] Hz, channels [" << channels.min << ", " << channels.max
                 << "], period [" << period.min << ", " << period.max << "] frames, buffer [" << buffer.min << ", "
                 << buffer.max << "] frames");
        LOG_WARN("  supports RW_INTERLEAVED: " << ((access.bits[SNDRV_PCM_ACCESS_RW_INTERLEAVED / 32] >> (SNDRV_PCM_ACCESS_RW_INTERLEAVED % 32)) & 1u)
                 << ", supports S16_LE: " << ((formats.bits[SNDRV_PCM_FORMAT_S16_LE / 32] >> (SNDRV_PCM_FORMAT_S16_LE % 32)) & 1u));
    } else {
        LOG_DEBUG("DirectAlsaPlayer: SNDRV_PCM_IOCTL_HW_REFINE probe failed for '" << probe_path << "': " << strerror(errno));
    }

    ::close(probe_fd);
}

} // namespace
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
    std::sort(devices.begin(), devices.end());
#endif
    return devices;
}

bool DirectAlsaPlayer::open(const AudioConfig& config, const std::string& device_name) {
    if (is_open_) {
        close();
    }

    config_ = config;
    device_path_ = device_name.empty() ? "/dev/snd/pcmC0D0p" : device_name;
    staging_buffer_.clear();

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
    std::string last_error;
    for (const auto& candidate : candidates) {
        int opened_fd = ::open(candidate.c_str(), O_RDWR | O_NONBLOCK);
        if (opened_fd < 0) {
            last_error = "open: " + std::string(strerror(errno));
            continue;
        }

        // Set hardware parameters via ioctl
        struct snd_pcm_hw_params params;
        init_any_params(params);

        // Access: RW interleaved (required for SNDRV_PCM_IOCTL_WRITEI_FRAMES)
        set_param_value(params, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);

        // Format: S16_LE
        set_param_value(params, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);

        // Channels (exact - stream is stereo)
        set_param_value(params, SNDRV_PCM_HW_PARAM_CHANNELS, config_.channels);

        // Rate (exact - this path performs no resampling)
        set_param_value(params, SNDRV_PCM_HW_PARAM_RATE, config_.sample_rate);

        const unsigned int period_req = config_.frames_per_packet > 0 ? static_cast<unsigned int>(config_.frames_per_packet) : 240;
        const unsigned int buffer_req = period_req * 4;

        params.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = period_req;
        params.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = period_req * 2;
        params.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = buffer_req;
        params.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = buffer_req * 4;

        if (ioctl(opened_fd, SNDRV_PCM_IOCTL_HW_PARAMS, &params) < 0) {
            last_error = "hw_params: " + std::string(strerror(errno));
            LOG_DEBUG("DirectAlsaPlayer: SNDRV_PCM_IOCTL_HW_PARAMS with requested period/buffer failed for candidate "
                      << candidate << ": " << strerror(errno) << ". Retrying with unconstrained period/buffer...");

            init_any_params(params);
            set_param_value(params, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
            set_param_value(params, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
            set_param_value(params, SNDRV_PCM_HW_PARAM_CHANNELS, config_.channels);
            set_param_value(params, SNDRV_PCM_HW_PARAM_RATE, config_.sample_rate);

            if (ioctl(opened_fd, SNDRV_PCM_IOCTL_HW_PARAMS, &params) < 0) {
                last_error = "hw_params (relaxed): " + std::string(strerror(errno));
                LOG_DEBUG("DirectAlsaPlayer: SNDRV_PCM_IOCTL_HW_PARAMS failed for candidate " << candidate << ": " << strerror(errno));
                ::close(opened_fd);
                continue;
            }
        }

        // The kernel copies the chosen configuration back into 'params' (the
        // ioctl is _IOWR). Read back the values it actually selected.
        period_size_frames_ = params.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
        buffer_size_frames_ = params.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;

        // Software parameters configuration
        struct snd_pcm_sw_params swparams;
        std::memset(&swparams, 0, sizeof(swparams));
        swparams.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
        swparams.period_step = 1;
        swparams.avail_min = (period_size_frames_ > 0) ? period_size_frames_ : 240;
        swparams.start_threshold = (period_size_frames_ > 0) ? period_size_frames_ : 240;
        swparams.stop_threshold = (buffer_size_frames_ > 0) ? buffer_size_frames_ : 4096;
        swparams.silence_threshold = 0;
        swparams.silence_size = 0;
        swparams.boundary = (buffer_size_frames_ > 0) ? buffer_size_frames_ : 4096;
        while (swparams.boundary * 2 <= ULONG_MAX - ((buffer_size_frames_ > 0) ? buffer_size_frames_ : 4096)) {
            swparams.boundary *= 2;
        }

        if (ioctl(opened_fd, SNDRV_PCM_IOCTL_SW_PARAMS, &swparams) < 0) {
            LOG_DEBUG("DirectAlsaPlayer: SNDRV_PCM_IOCTL_SW_PARAMS returned non-zero (continuing): " << strerror(errno));
        }

        // Prepare PCM
        if (ioctl(opened_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
            last_error = "prepare: " + std::string(strerror(errno));
            LOG_DEBUG("DirectAlsaPlayer: SNDRV_PCM_IOCTL_PREPARE failed for candidate " << candidate << ": " << strerror(errno));
            ::close(opened_fd);
            continue;
        }

        // Restore blocking mode for audio writes
        int flags = fcntl(opened_fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(opened_fd, F_SETFL, flags & ~O_NONBLOCK);
        }

        fd_ = opened_fd;
        device_path_ = candidate;
        success = true;
        break;
    }

    if (!success) {
        log_device_capabilities(candidates, last_error);
        LOG_ERROR("DirectAlsaPlayer: Failed to open and configure any fallback kernel PCM devices in /dev/snd/ (Last error: " << last_error << ")");
        return false;
    }

    is_open_ = true;
    LOG_INFO("DirectAlsaPlayer: Successfully opened and configured kernel ALSA device '" << device_path_
             << "' (" << config_.to_string() << ", period " << period_size_frames_
             << " frames, buffer " << buffer_size_frames_ << " frames)");
    return true;
#else
    LOG_INFO("DirectAlsaPlayer: Mock open on non-Linux platform");
    is_open_ = true;
    return true;
#endif
}

void DirectAlsaPlayer::close() {
    is_open_ = false;
#if defined(__linux__) || defined(__ANDROID__)
    int old_fd = fd_.exchange(-1);
    if (old_fd >= 0) {
        ioctl(old_fd, SNDRV_PCM_IOCTL_DROP);
        ::close(old_fd);
    }
#endif
    staging_buffer_.clear();
    LOG_INFO("DirectAlsaPlayer: Device closed");
}

bool DirectAlsaPlayer::is_open() const {
    return is_open_;
}

size_t DirectAlsaPlayer::write_frames(const void* pcm_data, size_t num_frames) {
    if (!is_open_ || !pcm_data || num_frames == 0) return 0;

#if defined(__linux__) || defined(__ANDROID__)
    int curr_fd = fd_.load();
    if (curr_fd < 0) return 0;

    const size_t channels = (config_.channels > 0) ? config_.channels : 2;
    const int16_t* src = reinterpret_cast<const int16_t*>(pcm_data);
    staging_buffer_.insert(staging_buffer_.end(), src, src + (num_frames * channels));

    size_t chunk_frames = (period_size_frames_ > 0) ? period_size_frames_ : num_frames;
    size_t chunk_samples = chunk_frames * channels;

    while (staging_buffer_.size() >= chunk_samples && is_open_ && fd_.load() >= 0) {
        int active_fd = fd_.load();
        if (active_fd < 0 || !is_open_) break;

        // Poll with 50ms timeout to avoid hanging if the hardware clock stalls or on shutdown
        struct pollfd pfd;
        pfd.fd = active_fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int poll_res = poll(&pfd, 1, 50);

        if (poll_res <= 0) {
            if (!is_open_ || fd_.load() < 0) break;
            if (poll_res == 0) {
                // Buffer full, device draining
                break;
            }
            if (errno == EINTR) continue;
            break;
        }

        active_fd = fd_.load();
        if (active_fd < 0 || !is_open_) break;

        struct snd_xferi xferi;
        xferi.result = 0;
        xferi.buf = staging_buffer_.data();
        xferi.frames = chunk_frames;

        int ret = ioctl(active_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xferi);
        if (ret < 0) {
            if (!is_open_ || fd_.load() < 0) break;
            if (errno == EPIPE) {
                // Underrun - prepare device again
                LOG_DEBUG("DirectAlsaPlayer: Kernel buffer underrun (XRUN), recovering...");
                ioctl(active_fd, SNDRV_PCM_IOCTL_PREPARE);
                ret = ioctl(active_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xferi);
            } else if (errno == ESTRPIPE) {
                // Suspended
                ioctl(active_fd, SNDRV_PCM_IOCTL_RESUME);
                ioctl(active_fd, SNDRV_PCM_IOCTL_PREPARE);
                ret = ioctl(active_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xferi);
            } else {
                ioctl(active_fd, SNDRV_PCM_IOCTL_PREPARE);
                ret = ioctl(active_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xferi);
            }
        }

        if (ret >= 0) {
            size_t written_frames = (xferi.result > 0) ? static_cast<size_t>(xferi.result) : chunk_frames;
            size_t written_samples = written_frames * channels;
            if (written_samples > staging_buffer_.size()) {
                written_samples = staging_buffer_.size();
            }
            staging_buffer_.erase(staging_buffer_.begin(), staging_buffer_.begin() + written_samples);
        } else {
            LOG_DEBUG("DirectAlsaPlayer: writei failed with errno " << errno << " (" << strerror(errno) << ")");
            staging_buffer_.clear();
            break;
        }
    }

    return num_frames;
#else
    return num_frames;
#endif
}

size_t DirectAlsaPlayer::get_buffer_delay_frames() const {
    if (!is_open_) return 0;
#if defined(__linux__) || defined(__ANDROID__)
    int curr_fd = fd_.load();
    if (curr_fd < 0) return 0;
    snd_pcm_sframes_t delay = 0;
    if (ioctl(curr_fd, SNDRV_PCM_IOCTL_DELAY, &delay) >= 0) {
        return delay > 0 ? static_cast<size_t>(delay) : 0;
    }
#endif
    return 0;
}

void DirectAlsaPlayer::flush() {
    if (!is_open_) return;
#if defined(__linux__) || defined(__ANDROID__)
    int curr_fd = fd_.load();
    if (curr_fd >= 0) {
        ioctl(curr_fd, SNDRV_PCM_IOCTL_DROP);
        ioctl(curr_fd, SNDRV_PCM_IOCTL_PREPARE);
    }
#endif
    staging_buffer_.clear();
}

std::string DirectAlsaPlayer::get_device_name() const {
    return device_path_;
}

} // namespace audiorouter
