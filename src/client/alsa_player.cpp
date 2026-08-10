#include "alsa_player.hpp"
#include "../common/logger.hpp"

#include <cstring>

#if defined(__linux__) || defined(__ANDROID__)
    #include <dlfcn.h>

namespace {
    void quiet_alsa_error_handler(const char* file, int line, const char* function, int err, const char* fmt, ...) {
        (void)file;
        (void)line;
        (void)function;
        (void)err;
        (void)fmt;
    }
}
#endif

namespace audiorouter {

struct AlsaPlayer::Impl {
#if defined(__linux__) || defined(__ANDROID__)
    void* lib_handle = nullptr;
    void* pcm_handle = nullptr;

    // Function pointers for dynamic ALSA loading
    int (*snd_pcm_open)(void** pcm, const char* name, int stream, int mode) = nullptr;
    int (*snd_pcm_close)(void* pcm) = nullptr;
    int (*snd_pcm_hw_params_malloc)(void** ptr) = nullptr;
    void (*snd_pcm_hw_params_free)(void* ptr) = nullptr;
    int (*snd_pcm_hw_params_any)(void* pcm, void* params) = nullptr;
    int (*snd_pcm_hw_params_set_access)(void* pcm, void* params, int access) = nullptr;
    int (*snd_pcm_hw_params_set_format)(void* pcm, void* params, int format) = nullptr;
    int (*snd_pcm_hw_params_set_channels)(void* pcm, void* params, unsigned int val) = nullptr;
    int (*snd_pcm_hw_params_set_rate_near)(void* pcm, void* params, unsigned int* val, int* dir) = nullptr;
    int (*snd_pcm_hw_params_set_period_size_near)(void* pcm, void* params, unsigned long* val, int* dir) = nullptr;
    int (*snd_pcm_hw_params_set_buffer_size_near)(void* pcm, void* params, unsigned long* val) = nullptr;
    int (*snd_pcm_hw_params)(void* pcm, void* params) = nullptr;
    long (*snd_pcm_writei)(void* pcm, const void* buffer, unsigned long size) = nullptr;
    int (*snd_pcm_recover)(void* pcm, int err, int silent) = nullptr;
    int (*snd_pcm_prepare)(void* pcm) = nullptr;
    int (*snd_pcm_drop)(void* pcm) = nullptr;
    int (*snd_pcm_delay)(void* pcm, long* delayp) = nullptr;
    const char* (*snd_strerror)(int errnum) = nullptr;
    int (*snd_lib_error_set_handler)(void (*handler)(const char *file, int line, const char *function, int err, const char *fmt, ...)) = nullptr;

    bool load_libasound() {
        if (lib_handle) return true;

        const char* lib_names[] = {
            "libasound.so.2",
            "libasound.so",
            "/data/data/com.termux/files/usr/lib/libasound.so.2",
            "/data/data/com.termux/files/usr/lib/libasound.so",
            "/system/lib64/libasound.so",
            "/system/lib/libasound.so",
            nullptr
        };

        for (int i = 0; lib_names[i] != nullptr; ++i) {
            lib_handle = dlopen(lib_names[i], RTLD_NOW | RTLD_LOCAL);
            if (lib_handle) {
                LOG_INFO("AlsaPlayer: Loaded ALSA library: " << lib_names[i]);
                break;
            }
        }

        if (!lib_handle) {
            LOG_DEBUG("AlsaPlayer: libasound.so not found on system");
            return false;
        }

        #define LOAD_SYM(name) \
            name = (decltype(name))dlsym(lib_handle, #name); \
            if (!name) { \
                LOG_WARN("AlsaPlayer: Missing symbol " #name " in libasound"); \
                dlclose(lib_handle); \
                lib_handle = nullptr; \
                return false; \
            }

        LOAD_SYM(snd_pcm_open);
        LOAD_SYM(snd_pcm_close);
        LOAD_SYM(snd_pcm_hw_params_malloc);
        LOAD_SYM(snd_pcm_hw_params_free);
        LOAD_SYM(snd_pcm_hw_params_any);
        LOAD_SYM(snd_pcm_hw_params_set_access);
        LOAD_SYM(snd_pcm_hw_params_set_format);
        LOAD_SYM(snd_pcm_hw_params_set_channels);
        LOAD_SYM(snd_pcm_hw_params_set_rate_near);
        LOAD_SYM(snd_pcm_hw_params_set_period_size_near);
        LOAD_SYM(snd_pcm_hw_params_set_buffer_size_near);
        LOAD_SYM(snd_pcm_hw_params);
        LOAD_SYM(snd_pcm_writei);
        LOAD_SYM(snd_pcm_recover);
        LOAD_SYM(snd_pcm_prepare);
        LOAD_SYM(snd_pcm_drop);
        LOAD_SYM(snd_pcm_delay);
        LOAD_SYM(snd_strerror);

        #undef LOAD_SYM

        snd_lib_error_set_handler = (decltype(snd_lib_error_set_handler))dlsym(lib_handle, "snd_lib_error_set_handler");
        if (snd_lib_error_set_handler) {
            snd_lib_error_set_handler(quiet_alsa_error_handler);
        }

        return true;
    }
#endif
};

AlsaPlayer::AlsaPlayer()
    : impl_(std::make_unique<Impl>()),
      direct_fallback_(std::make_unique<DirectAlsaPlayer>()),
      using_direct_fallback_(false),
      is_open_(false),
      device_name_("default") {}

AlsaPlayer::~AlsaPlayer() {
    close();
}

std::vector<std::string> AlsaPlayer::get_available_devices() {
    std::vector<std::string> devices;
    devices.push_back("default");
    devices.push_back("plughw:0,0");
    devices.push_back("hw:0,0");

    auto kernel_devs = DirectAlsaPlayer::enumerate_kernel_pcm_devices();
    for (const auto& dev : kernel_devs) {
        devices.push_back(dev);
    }
    return devices;
}

bool AlsaPlayer::open(const AudioConfig& config, const std::string& device_name) {
    if (is_open_) {
        close();
    }

    config_ = config;
    device_name_ = device_name.empty() ? "default" : device_name;
    using_direct_fallback_ = false;

    // If device starts with /dev/snd/ or direct:, use direct kernel driver immediately
    if (device_name_.rfind("/dev/snd/", 0) == 0 || device_name_.rfind("direct:", 0) == 0) {
        std::string actual_path = device_name_;
        if (actual_path.rfind("direct:", 0) == 0) {
            actual_path = actual_path.substr(7);
        }
        LOG_INFO("Using Direct Kernel ALSA device node: " << actual_path);
        using_direct_fallback_ = true;
        is_open_ = direct_fallback_->open(config, actual_path);
        return is_open_;
    }

#if defined(__linux__) || defined(__ANDROID__)
    // If device_name is default and direct kernel PCM nodes are detected, prefer Direct ALSA
    auto kernel_devs = DirectAlsaPlayer::enumerate_kernel_pcm_devices();
    if (!kernel_devs.empty() && device_name_ == "default") {
        LOG_INFO("AlsaPlayer: Direct ALSA nodes detected in /dev/snd/, attempting direct hardware playback...");
        using_direct_fallback_ = true;
        if (direct_fallback_->open(config, kernel_devs.front())) {
            is_open_ = true;
            return true;
        }
        using_direct_fallback_ = false;
    }

    if (impl_->load_libasound()) {
        // Try the requested device first, then fall back to other libasound names
        std::vector<std::string> dev_candidates;
        dev_candidates.push_back(device_name_);
        if (device_name_ != "plughw:0,0") {
            dev_candidates.push_back("plughw:0,0");
        }
        if (device_name_ != "hw:0,0") {
            dev_candidates.push_back("hw:0,0");
        }

        for (const auto& dev : dev_candidates) {
            if (try_open_via_libasound(config, dev)) {
                return true;
            }
        }
    }
#endif

    // Fallback to Direct Kernel ALSA driver (/dev/snd/pcmC0D0p)
    LOG_INFO("AlsaPlayer: Falling back to Direct Kernel ALSA driver on /dev/snd/pcmC0D0p");
    using_direct_fallback_ = true;
    is_open_ = direct_fallback_->open(config, "/dev/snd/pcmC0D0p");
    if (!is_open_) {
        LOG_WARN("AlsaPlayer: Could not open direct kernel ALSA device. Check Termux root permissions (run 'su').");
    }
    return is_open_;
}

bool AlsaPlayer::try_open_via_libasound(const AudioConfig& config, const std::string& device_name) {
#if defined(__linux__) || defined(__ANDROID__)
    const int SND_PCM_STREAM_PLAYBACK_VAL = 0;
    const int SND_PCM_ACCESS_RW_INTERLEAVED_VAL = 3;
    const int SND_PCM_FORMAT_S16_LE_VAL = 2;

    int err = impl_->snd_pcm_open(&impl_->pcm_handle, device_name.c_str(), SND_PCM_STREAM_PLAYBACK_VAL, 0);
    if (err < 0) {
        LOG_WARN("AlsaPlayer: snd_pcm_open failed for '" << device_name << "': "
                 << impl_->snd_strerror(err));
        return false;
    }

    void* hw_params = nullptr;
    impl_->snd_pcm_hw_params_malloc(&hw_params);
    impl_->snd_pcm_hw_params_any(impl_->pcm_handle, hw_params);
    impl_->snd_pcm_hw_params_set_access(impl_->pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED_VAL);
    impl_->snd_pcm_hw_params_set_format(impl_->pcm_handle, hw_params, SND_PCM_FORMAT_S16_LE_VAL);
    impl_->snd_pcm_hw_params_set_channels(impl_->pcm_handle, hw_params, config.channels);

    unsigned int rate = config.sample_rate;
    int dir = 0;
    impl_->snd_pcm_hw_params_set_rate_near(impl_->pcm_handle, hw_params, &rate, &dir);

    unsigned long period_size = config.frames_per_packet > 0 ? config.frames_per_packet : 240;
    impl_->snd_pcm_hw_params_set_period_size_near(impl_->pcm_handle, hw_params, &period_size, &dir);

    unsigned long buffer_size = period_size * 4;
    impl_->snd_pcm_hw_params_set_buffer_size_near(impl_->pcm_handle, hw_params, &buffer_size);

    err = impl_->snd_pcm_hw_params(impl_->pcm_handle, hw_params);
    impl_->snd_pcm_hw_params_free(hw_params);

    if (err < 0) {
        LOG_WARN("AlsaPlayer: snd_pcm_hw_params failed for '" << device_name << "': " << impl_->snd_strerror(err));
        impl_->snd_pcm_close(impl_->pcm_handle);
        impl_->pcm_handle = nullptr;
        return false;
    }

    impl_->snd_pcm_prepare(impl_->pcm_handle);
    is_open_ = true;
    device_name_ = device_name;
    LOG_INFO("AlsaPlayer: Successfully opened ALSA PCM device '" << device_name
             << "' via libasound (" << config.to_string() << ")");
    return true;
#else
    (void)config;
    (void)device_name;
    return false;
#endif
}

void AlsaPlayer::close() {
    if (!is_open_) return;

    if (using_direct_fallback_) {
        direct_fallback_->close();
    } else {
#if defined(__linux__) || defined(__ANDROID__)
        if (impl_->pcm_handle && impl_->snd_pcm_close) {
            impl_->snd_pcm_drop(impl_->pcm_handle);
            impl_->snd_pcm_close(impl_->pcm_handle);
            impl_->pcm_handle = nullptr;
        }
#endif
    }
    is_open_ = false;
    LOG_INFO("AlsaPlayer: Closed");
}

bool AlsaPlayer::is_open() const {
    return is_open_;
}

size_t AlsaPlayer::write_frames(const void* pcm_data, size_t num_frames) {
    if (!is_open_ || !pcm_data || num_frames == 0) return 0;

    if (using_direct_fallback_) {
        return direct_fallback_->write_frames(pcm_data, num_frames);
    }

#if defined(__linux__) || defined(__ANDROID__)
    if (!impl_->pcm_handle || !impl_->snd_pcm_writei) return 0;

    long written = impl_->snd_pcm_writei(impl_->pcm_handle, pcm_data, num_frames);
    if (written < 0) {
        // Recover from XRUN / underrun
        if (impl_->snd_pcm_recover) {
            written = impl_->snd_pcm_recover(impl_->pcm_handle, static_cast<int>(written), 1);
            if (written >= 0) {
                written = impl_->snd_pcm_writei(impl_->pcm_handle, pcm_data, num_frames);
            }
        }
    }

    return written > 0 ? static_cast<size_t>(written) : 0;
#else
    return num_frames;
#endif
}

size_t AlsaPlayer::get_buffer_delay_frames() const {
    if (!is_open_) return 0;

    if (using_direct_fallback_) {
        return direct_fallback_->get_buffer_delay_frames();
    }

#if defined(__linux__) || defined(__ANDROID__)
    if (impl_->pcm_handle && impl_->snd_pcm_delay) {
        long delay = 0;
        if (impl_->snd_pcm_delay(impl_->pcm_handle, &delay) == 0 && delay > 0) {
            return static_cast<size_t>(delay);
        }
    }
#endif
    return 0;
}

void AlsaPlayer::flush() {
    if (!is_open_) return;

    if (using_direct_fallback_) {
        direct_fallback_->flush();
    } else {
#if defined(__linux__) || defined(__ANDROID__)
        if (impl_->pcm_handle) {
            if (impl_->snd_pcm_drop) impl_->snd_pcm_drop(impl_->pcm_handle);
            if (impl_->snd_pcm_prepare) impl_->snd_pcm_prepare(impl_->pcm_handle);
        }
#endif
    }
}

std::string AlsaPlayer::get_device_name() const {
    if (using_direct_fallback_) {
        return direct_fallback_->get_device_name();
    }
    return device_name_;
}

} // namespace audiorouter
