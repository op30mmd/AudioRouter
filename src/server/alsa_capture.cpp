#include "alsa_capture.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <cerrno>
#include <cstring>
#include <vector>

#if defined(__linux__) && !defined(__ANDROID__)
    #include <dlfcn.h>

namespace {
    void quiet_alsa_capture_error_handler(const char* /*file*/, int /*line*/,
                                          const char* /*function*/, int /*err*/,
                                          const char* /*fmt*/, ...) {}
}
#endif

namespace audiorouter {

struct AlsaCapture::Impl {
#if defined(__linux__) && !defined(__ANDROID__)
    void* lib_handle = nullptr;
    void* pcm_handle = nullptr;

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
    long (*snd_pcm_readi)(void* pcm, void* buffer, unsigned long size) = nullptr;
    int (*snd_pcm_recover)(void* pcm, int err, int silent) = nullptr;
    int (*snd_pcm_prepare)(void* pcm) = nullptr;
    int (*snd_pcm_drop)(void* pcm) = nullptr;
    const char* (*snd_strerror)(int errnum) = nullptr;
    int (*snd_lib_error_set_handler)(void (*handler)(const char* file, int line, const char* function, int err, const char* fmt, ...)) = nullptr;

    bool load_libasound() {
        if (lib_handle) return true;

        const char* lib_names[] = {"libasound.so.2", "libasound.so", nullptr};
        for (int i = 0; lib_names[i] != nullptr; ++i) {
            lib_handle = ::dlopen(lib_names[i], RTLD_NOW | RTLD_LOCAL);
            if (lib_handle) break;
        }
        if (!lib_handle) return false;

        #define LOAD_SYM(name) \
            name = reinterpret_cast<decltype(name)>(::dlsym(lib_handle, #name)); \
            if (!name) { \
                ::dlclose(lib_handle); \
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
        LOAD_SYM(snd_pcm_readi);
        LOAD_SYM(snd_pcm_recover);
        LOAD_SYM(snd_pcm_prepare);
        LOAD_SYM(snd_pcm_drop);
        LOAD_SYM(snd_strerror);

        #undef LOAD_SYM

        snd_lib_error_set_handler = reinterpret_cast<decltype(snd_lib_error_set_handler)>(
            ::dlsym(lib_handle, "snd_lib_error_set_handler"));
        if (snd_lib_error_set_handler) {
            snd_lib_error_set_handler(quiet_alsa_capture_error_handler);
        }
        return true;
    }

    ~Impl() {
        if (lib_handle) {
            ::dlclose(lib_handle);
            lib_handle = nullptr;
        }
    }
#endif
};

AlsaCapture::AlsaCapture(std::string device_spec)
    : impl_(std::make_unique<Impl>()),
      device_name_(parse_device_spec(device_spec)),
      is_running_(false) {}

AlsaCapture::~AlsaCapture() {
    stop();
}

std::string AlsaCapture::parse_device_spec(const std::string& device_spec) {
    if (device_spec.empty() || device_spec == "auto" || device_spec == "alsa") {
        return "default";
    }
    if (device_spec.rfind("alsa:", 0) == 0) {
        std::string rest = device_spec.substr(5);
        return rest.empty() ? "default" : rest;
    }
    return device_spec;
}

bool AlsaCapture::is_supported() {
#if defined(__linux__) && !defined(__ANDROID__)
    Impl probe;
    return probe.load_libasound();
#else
    return false;
#endif
}

bool AlsaCapture::start(const AudioConfig& desired_config, AudioConfig& actual_config) {
    if (is_running_) {
        actual_config = actual_config_;
        return true;
    }

#if defined(__linux__) && !defined(__ANDROID__)
    if (!impl_->load_libasound()) {
        LOG_ERROR("AlsaCapture: libasound.so.2 not found on system.");
        return false;
    }

    constexpr int kSndPcmStreamCapture = 1;
    constexpr int kSndPcmAccessRwInterleaved = 3;
    constexpr int kSndPcmFormatS16Le = 2;

    int err = impl_->snd_pcm_open(&impl_->pcm_handle, device_name_.c_str(), kSndPcmStreamCapture, 0);
    if (err < 0 || !impl_->pcm_handle) {
        LOG_ERROR("AlsaCapture: Failed to open ALSA capture device '" << device_name_
                  << "': " << impl_->snd_strerror(err));
        impl_->pcm_handle = nullptr;
        return false;
    }

    void* hw_params = nullptr;
    if (impl_->snd_pcm_hw_params_malloc(&hw_params) < 0 || !hw_params) {
        impl_->snd_pcm_close(impl_->pcm_handle);
        impl_->pcm_handle = nullptr;
        return false;
    }

    unsigned int rate = desired_config.sample_rate > 0 ? desired_config.sample_rate : 48000;
    unsigned int channels = desired_config.channels > 0 ? desired_config.channels : 2;
    unsigned long period_frames = desired_config.frames_per_packet > 0 ? desired_config.frames_per_packet : 240;
    unsigned long buffer_frames = period_frames * 4;
    int dir = 0;

    bool ok = true;
    ok = ok && impl_->snd_pcm_hw_params_any(impl_->pcm_handle, hw_params) >= 0;
    ok = ok && impl_->snd_pcm_hw_params_set_access(impl_->pcm_handle, hw_params, kSndPcmAccessRwInterleaved) >= 0;
    ok = ok && impl_->snd_pcm_hw_params_set_format(impl_->pcm_handle, hw_params, kSndPcmFormatS16Le) >= 0;
    ok = ok && impl_->snd_pcm_hw_params_set_channels(impl_->pcm_handle, hw_params, channels) >= 0;
    ok = ok && impl_->snd_pcm_hw_params_set_rate_near(impl_->pcm_handle, hw_params, &rate, &dir) >= 0;
    ok = ok && impl_->snd_pcm_hw_params_set_period_size_near(impl_->pcm_handle, hw_params, &period_frames, &dir) >= 0;
    ok = ok && impl_->snd_pcm_hw_params_set_buffer_size_near(impl_->pcm_handle, hw_params, &buffer_frames) >= 0;
    ok = ok && impl_->snd_pcm_hw_params(impl_->pcm_handle, hw_params) >= 0;
    impl_->snd_pcm_hw_params_free(hw_params);

    if (!ok) {
        LOG_ERROR("AlsaCapture: Failed to configure ALSA hardware params on '" << device_name_ << "'");
        impl_->snd_pcm_close(impl_->pcm_handle);
        impl_->pcm_handle = nullptr;
        return false;
    }

    impl_->snd_pcm_prepare(impl_->pcm_handle);

    actual_config_.sample_rate = rate;
    actual_config_.channels = static_cast<uint16_t>(channels);
    actual_config_.format = AudioSampleFormat::PCM_S16LE;
    actual_config_.frames_per_packet = static_cast<uint32_t>(period_frames);
    actual_config = actual_config_;

    is_running_ = true;
    capture_thread_ = std::thread(&AlsaCapture::capture_thread_func, this);
    LOG_INFO("AlsaCapture: Capture active on '" << device_name_ << "' ("
             << actual_config_.to_string() << ")");
    return true;
#else
    (void)desired_config;
    (void)actual_config;
    return false;
#endif
}

void AlsaCapture::stop() {
    if (!is_running_) return;
    is_running_ = false;
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
#if defined(__linux__) && !defined(__ANDROID__)
    if (impl_->pcm_handle) {
        impl_->snd_pcm_drop(impl_->pcm_handle);
        impl_->snd_pcm_close(impl_->pcm_handle);
        impl_->pcm_handle = nullptr;
    }
#endif
    LOG_INFO("AlsaCapture: Capture stopped");
}

bool AlsaCapture::is_capturing() const {
    return is_running_;
}

void AlsaCapture::set_audio_callback(AudioCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_ = std::move(cb);
}

std::string AlsaCapture::get_device_name() const {
    return device_name_;
}

void AlsaCapture::capture_thread_func() {
#if defined(__linux__) && !defined(__ANDROID__)
    const unsigned long frames_per_block = actual_config_.frames_per_packet;
    const size_t bytes_per_block = static_cast<size_t>(frames_per_block) * actual_config_.bytes_per_frame();
    std::vector<uint8_t> buf(bytes_per_block);

    while (is_running_) {
        long read_frames = impl_->snd_pcm_readi(impl_->pcm_handle, buf.data(), frames_per_block);
        if (read_frames < 0) {
            if (!is_running_) break;
            int rec = impl_->snd_pcm_recover(impl_->pcm_handle, static_cast<int>(read_frames), 1);
            if (rec < 0) {
                LOG_WARN("AlsaCapture: snd_pcm_readi error: "
                         << impl_->snd_strerror(static_cast<int>(read_frames)));
                sleep_ms(10);
            }
            continue;
        }

        if (read_frames > 0) {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (callback_) {
                callback_(buf.data(), static_cast<size_t>(read_frames), actual_config_);
            }
        }
    }
#endif
}

} // namespace audiorouter
