#include "audio_endpoint_control.hpp"
#include "../common/logger.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(_WIN32)
    #include <windows.h>
    #include <mmdeviceapi.h>
    #include <endpointvolume.h>
#elif defined(__linux__) && !defined(__ANDROID__)
    #include <dlfcn.h>
#endif

namespace audiorouter {

namespace {
#if defined(__linux__) && !defined(__ANDROID__)
constexpr int kAlsaFrontLeft = 0; // SND_MIXER_SCHN_FRONT_LEFT

bool is_preferred_mixer_selem(const char* name) {
    if (!name) return false;
    return std::strcmp(name, "Master") == 0 ||
           std::strcmp(name, "Speaker") == 0 ||
           std::strcmp(name, "Headphone") == 0 ||
           std::strcmp(name, "Front") == 0 ||
           std::strcmp(name, "PCM") == 0;
}
#endif
} // namespace

struct AudioEndpointControl::Impl {
#if defined(_WIN32)
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioEndpointVolume* endpoint_volume = nullptr;
    bool com_initialized = false;
#elif defined(__linux__) && !defined(__ANDROID__)
    void* lib_handle = nullptr;
    void* mixer = nullptr;
    void* elem = nullptr;
    long vol_min = 0;
    long vol_max = 100;
    bool has_switch = false;
    bool has_volume = false;

    int (*snd_mixer_open)(void** mixer, int mode) = nullptr;
    int (*snd_mixer_close)(void* mixer) = nullptr;
    int (*snd_mixer_attach)(void* mixer, const char* name) = nullptr;
    int (*snd_mixer_selem_register)(void* mixer, void* options, void** classp) = nullptr;
    int (*snd_mixer_load)(void* mixer) = nullptr;
    void* (*snd_mixer_first_elem)(void* mixer) = nullptr;
    void* (*snd_mixer_elem_next)(void* elem) = nullptr;
    const char* (*snd_mixer_selem_get_name)(void* elem) = nullptr;
    int (*snd_mixer_selem_has_playback_volume)(void* elem) = nullptr;
    int (*snd_mixer_selem_has_playback_switch)(void* elem) = nullptr;
    int (*snd_mixer_selem_get_playback_volume_range)(void* elem, long* min, long* max) = nullptr;
    int (*snd_mixer_selem_get_playback_volume)(void* elem, int channel, long* value) = nullptr;
    int (*snd_mixer_selem_set_playback_volume_all)(void* elem, long value) = nullptr;
    int (*snd_mixer_selem_get_playback_switch)(void* elem, int channel, int* value) = nullptr;
    int (*snd_mixer_selem_set_playback_switch_all)(void* elem, int value) = nullptr;
    int (*snd_mixer_handle_events)(void* mixer) = nullptr;

    bool load_libasound() {
        if (lib_handle) return true;
        const char* names[] = {"libasound.so.2", "libasound.so", nullptr};
        for (int i = 0; names[i] != nullptr; ++i) {
            lib_handle = ::dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
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

        LOAD_SYM(snd_mixer_open);
        LOAD_SYM(snd_mixer_close);
        LOAD_SYM(snd_mixer_attach);
        LOAD_SYM(snd_mixer_selem_register);
        LOAD_SYM(snd_mixer_load);
        LOAD_SYM(snd_mixer_first_elem);
        LOAD_SYM(snd_mixer_elem_next);
        LOAD_SYM(snd_mixer_selem_get_name);
        LOAD_SYM(snd_mixer_selem_has_playback_volume);
        LOAD_SYM(snd_mixer_selem_has_playback_switch);
        LOAD_SYM(snd_mixer_selem_get_playback_volume_range);
        LOAD_SYM(snd_mixer_selem_get_playback_volume);
        LOAD_SYM(snd_mixer_selem_set_playback_volume_all);
        LOAD_SYM(snd_mixer_selem_get_playback_switch);
        LOAD_SYM(snd_mixer_selem_set_playback_switch_all);
        LOAD_SYM(snd_mixer_handle_events);

        #undef LOAD_SYM
        return true;
    }

    void close_mixer() {
        elem = nullptr;
        if (mixer && snd_mixer_close) {
            snd_mixer_close(mixer);
        }
        mixer = nullptr;
        has_switch = false;
        has_volume = false;
    }

    ~Impl() {
        close_mixer();
        if (lib_handle) {
            ::dlclose(lib_handle);
            lib_handle = nullptr;
        }
    }
#else
    bool dummy_muted = false;
    float dummy_volume = 1.0f;
#endif
};

AudioEndpointControl::AudioEndpointControl()
    : impl_(std::make_unique<Impl>()),
      is_silenced_by_us_(false),
      prev_mute_state_(false),
      prev_volume_level_(1.0f),
      active_mute_method_(MuteMethod::EndpointMute) {}

AudioEndpointControl::~AudioEndpointControl() {
    shutdown();
}

bool AudioEndpointControl::init() {
    std::lock_guard<std::mutex> lock(mutex_);

#if defined(_WIN32)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        impl_->com_initialized = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        impl_->com_initialized = false; // COM already initialized in different mode, continue anyway
    } else {
        LOG_ERROR("AudioEndpointControl: CoInitializeEx failed with HRESULT: 0x" << std::hex << hr);
        return false;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&impl_->enumerator);
    if (FAILED(hr) || !impl_->enumerator) {
        LOG_ERROR("AudioEndpointControl: Failed to create MMDeviceEnumerator. HRESULT: 0x" << std::hex << hr);
        return false;
    }

    hr = impl_->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &impl_->device);
    if (FAILED(hr) || !impl_->device) {
        LOG_ERROR("AudioEndpointControl: Failed to get default render endpoint. HRESULT: 0x" << std::hex << hr);
        return false;
    }

    hr = impl_->device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&impl_->endpoint_volume);
    if (FAILED(hr) || !impl_->endpoint_volume) {
        LOG_ERROR("AudioEndpointControl: Failed to activate IAudioEndpointVolume. HRESULT: 0x" << std::hex << hr);
        return false;
    }

    LOG_INFO("AudioEndpointControl: Successfully initialized Windows endpoint volume control");
    return true;
#elif defined(__linux__) && !defined(__ANDROID__)
    if (impl_->mixer && impl_->elem) {
        return true;
    }
    if (!impl_->load_libasound()) {
        LOG_DEBUG("AudioEndpointControl: libasound.so.2 not available; speaker muting disabled");
        return false;
    }

    // Try "default" first, then "hw:0" for systems where default is a virtual/pulse mixer
    // without hardware playback switches (muting ALSA hardware keeps PulseAudio .monitor alive).
    const char* card_names[] = {"hw:0", "default", nullptr};
    for (int i = 0; card_names[i] != nullptr; ++i) {
        void* m = nullptr;
        if (impl_->snd_mixer_open(&m, 0) < 0 || !m) continue;
        if (impl_->snd_mixer_attach(m, card_names[i]) < 0 ||
            impl_->snd_mixer_selem_register(m, nullptr, nullptr) < 0 ||
            impl_->snd_mixer_load(m) < 0) {
            impl_->snd_mixer_close(m);
            continue;
        }

        void* best_elem = nullptr;
        for (void* e = impl_->snd_mixer_first_elem(m); e != nullptr; e = impl_->snd_mixer_elem_next(e)) {
            const char* name = impl_->snd_mixer_selem_get_name(e);
            if (!is_preferred_mixer_selem(name)) continue;
            if (impl_->snd_mixer_selem_has_playback_switch(e) ||
                impl_->snd_mixer_selem_has_playback_volume(e)) {
                best_elem = e;
                if (std::strcmp(name, "Master") == 0) break;
            }
        }

        if (best_elem) {
            impl_->mixer = m;
            impl_->elem = best_elem;
            impl_->has_switch = impl_->snd_mixer_selem_has_playback_switch(best_elem) != 0;
            impl_->has_volume = impl_->snd_mixer_selem_has_playback_volume(best_elem) != 0;
            if (impl_->has_volume) {
                impl_->snd_mixer_selem_get_playback_volume_range(best_elem, &impl_->vol_min, &impl_->vol_max);
            }
            LOG_INFO("AudioEndpointControl: Initialized Linux ALSA mixer control ('"
                     << impl_->snd_mixer_selem_get_name(best_elem) << "' on " << card_names[i] << ")");
            return true;
        }
        impl_->snd_mixer_close(m);
    }

    LOG_DEBUG("AudioEndpointControl: No controllable ALSA playback mixer element found");
    return false;
#else
    LOG_INFO("AudioEndpointControl: Mock endpoint volume controller initialized (Non-Windows)");
    return true;
#endif
}

void AudioEndpointControl::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_silenced_by_us_) {
#if defined(_WIN32)
        if (active_mute_method_ == MuteMethod::EndpointMute || active_mute_method_ == MuteMethod::Both) {
            if (impl_->endpoint_volume) {
                impl_->endpoint_volume->SetMute(prev_mute_state_ ? TRUE : FALSE, nullptr);
            }
        }
        if (active_mute_method_ == MuteMethod::VolumeZero || active_mute_method_ == MuteMethod::Both) {
            if (impl_->endpoint_volume) {
                impl_->endpoint_volume->SetMasterVolumeLevelScalar(prev_volume_level_, nullptr);
            }
        }
#elif defined(__linux__) && !defined(__ANDROID__)
        if (impl_->elem) {
            if ((active_mute_method_ == MuteMethod::EndpointMute || active_mute_method_ == MuteMethod::Both) &&
                impl_->has_switch) {
                impl_->snd_mixer_selem_set_playback_switch_all(impl_->elem, prev_mute_state_ ? 0 : 1);
            }
            if ((active_mute_method_ == MuteMethod::VolumeZero || active_mute_method_ == MuteMethod::Both) &&
                impl_->has_volume) {
                long raw = impl_->vol_min + static_cast<long>(std::lround(
                    prev_volume_level_ * static_cast<float>(impl_->vol_max - impl_->vol_min)));
                impl_->snd_mixer_selem_set_playback_volume_all(impl_->elem, raw);
            }
        }
#endif
        is_silenced_by_us_ = false;
    }

#if defined(_WIN32)
    if (impl_->endpoint_volume) {
        impl_->endpoint_volume->Release();
        impl_->endpoint_volume = nullptr;
    }
    if (impl_->device) {
        impl_->device->Release();
        impl_->device = nullptr;
    }
    if (impl_->enumerator) {
        impl_->enumerator->Release();
        impl_->enumerator = nullptr;
    }
    if (impl_->com_initialized) {
        CoUninitialize();
        impl_->com_initialized = false;
    }
#elif defined(__linux__) && !defined(__ANDROID__)
    impl_->close_mixer();
#endif
}

bool AudioEndpointControl::mute_pc_speaker(MuteMethod method) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_mute_method_ = method;

#if defined(_WIN32)
    if (!impl_->endpoint_volume) {
        LOG_WARN("AudioEndpointControl: Volume endpoint not initialized, attempting init...");
        // Re-init attempt
        if (!init()) return false;
    }

    BOOL current_mute = FALSE;
    float current_vol = 1.0f;
    impl_->endpoint_volume->GetMute(&current_mute);
    impl_->endpoint_volume->GetMasterVolumeLevelScalar(&current_vol);

    if (!is_silenced_by_us_) {
        prev_mute_state_ = (current_mute == TRUE);
        prev_volume_level_ = current_vol;
    }

    HRESULT hr = S_OK;
    if (method == MuteMethod::EndpointMute || method == MuteMethod::Both) {
        hr = impl_->endpoint_volume->SetMute(TRUE, nullptr);
        if (FAILED(hr)) {
            LOG_ERROR("AudioEndpointControl: Failed to set mute TRUE. HRESULT: 0x" << std::hex << hr);
        }
    }
    if (method == MuteMethod::VolumeZero || method == MuteMethod::Both) {
        hr = impl_->endpoint_volume->SetMasterVolumeLevelScalar(0.0f, nullptr);
        if (FAILED(hr)) {
            LOG_ERROR("AudioEndpointControl: Failed to set master volume 0.0. HRESULT: 0x" << std::hex << hr);
        }
    }

    is_silenced_by_us_ = true;
    LOG_INFO("AudioEndpointControl: PC Speaker made quiet (Previous Volume: "
             << static_cast<int>(prev_volume_level_ * 100) << "%, Previous Mute: "
             << (prev_mute_state_ ? "Muted" : "Unmuted") << ")");
    return SUCCEEDED(hr);
#elif defined(__linux__) && !defined(__ANDROID__)
    if (!impl_->elem) {
        return false;
    }

    impl_->snd_mixer_handle_events(impl_->mixer);

    if (!is_silenced_by_us_) {
        int sw = 1;
        if (impl_->has_switch &&
            impl_->snd_mixer_selem_get_playback_switch(impl_->elem, kAlsaFrontLeft, &sw) == 0) {
            prev_mute_state_ = (sw == 0);
        } else {
            prev_mute_state_ = false;
        }

        long raw = impl_->vol_max;
        if (impl_->has_volume && impl_->vol_max > impl_->vol_min &&
            impl_->snd_mixer_selem_get_playback_volume(impl_->elem, kAlsaFrontLeft, &raw) == 0) {
            prev_volume_level_ = static_cast<float>(raw - impl_->vol_min) /
                                 static_cast<float>(impl_->vol_max - impl_->vol_min);
        } else {
            prev_volume_level_ = 1.0f;
        }
    }

    bool ok = true;
    if (method == MuteMethod::EndpointMute || method == MuteMethod::Both) {
        if (impl_->has_switch) {
            ok = ok && (impl_->snd_mixer_selem_set_playback_switch_all(impl_->elem, 0) >= 0);
        } else if (method == MuteMethod::EndpointMute && impl_->has_volume) {
            // Fallback to volume 0 if mixer element has no mute switch
            ok = ok && (impl_->snd_mixer_selem_set_playback_volume_all(impl_->elem, impl_->vol_min) >= 0);
        }
    }
    if (method == MuteMethod::VolumeZero || method == MuteMethod::Both) {
        if (impl_->has_volume) {
            ok = ok && (impl_->snd_mixer_selem_set_playback_volume_all(impl_->elem, impl_->vol_min) >= 0);
        }
    }

    if (ok) {
        is_silenced_by_us_ = true;
        LOG_INFO("AudioEndpointControl: PC Speaker silenced (Previous Volume: "
                 << static_cast<int>(prev_volume_level_ * 100) << "%, Previous Mute: "
                 << (prev_mute_state_ ? "Muted" : "Unmuted") << ")");
    }
    return ok;
#else
    if (!is_silenced_by_us_) {
        prev_mute_state_ = impl_->dummy_muted;
        prev_volume_level_ = impl_->dummy_volume;
    }
    impl_->dummy_muted = true;
    impl_->dummy_volume = 0.0f;
    is_silenced_by_us_ = true;
    LOG_INFO("AudioEndpointControl [Mock]: PC Speaker silenced (Previous Vol: "
             << static_cast<int>(prev_volume_level_ * 100) << "%)");
    return true;
#endif
}

bool AudioEndpointControl::unmute_pc_speaker() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_silenced_by_us_) {
        return true; // Not silenced by us
    }

#if defined(_WIN32)
    if (!impl_->endpoint_volume) return false;

    if (active_mute_method_ == MuteMethod::EndpointMute || active_mute_method_ == MuteMethod::Both) {
        impl_->endpoint_volume->SetMute(prev_mute_state_ ? TRUE : FALSE, nullptr);
    }
    if (active_mute_method_ == MuteMethod::VolumeZero || active_mute_method_ == MuteMethod::Both) {
        impl_->endpoint_volume->SetMasterVolumeLevelScalar(prev_volume_level_, nullptr);
    }

    is_silenced_by_us_ = false;
    LOG_INFO("AudioEndpointControl: PC Speaker restored to previous state (Volume: "
             << static_cast<int>(prev_volume_level_ * 100) << "%, Mute: "
             << (prev_mute_state_ ? "Muted" : "Unmuted") << ")");
    return true;
#elif defined(__linux__) && !defined(__ANDROID__)
    if (!impl_->elem) return false;

    if (active_mute_method_ == MuteMethod::EndpointMute || active_mute_method_ == MuteMethod::Both) {
        if (impl_->has_switch) {
            impl_->snd_mixer_selem_set_playback_switch_all(impl_->elem, prev_mute_state_ ? 0 : 1);
        } else if (active_mute_method_ == MuteMethod::EndpointMute && impl_->has_volume) {
            long raw = impl_->vol_min + static_cast<long>(std::lround(
                prev_volume_level_ * static_cast<float>(impl_->vol_max - impl_->vol_min)));
            impl_->snd_mixer_selem_set_playback_volume_all(impl_->elem, raw);
        }
    }
    if (active_mute_method_ == MuteMethod::VolumeZero || active_mute_method_ == MuteMethod::Both) {
        if (impl_->has_volume) {
            long raw = impl_->vol_min + static_cast<long>(std::lround(
                prev_volume_level_ * static_cast<float>(impl_->vol_max - impl_->vol_min)));
            impl_->snd_mixer_selem_set_playback_volume_all(impl_->elem, raw);
        }
    }

    is_silenced_by_us_ = false;
    LOG_INFO("AudioEndpointControl: PC Speaker restored to previous state (Volume: "
             << static_cast<int>(prev_volume_level_ * 100) << "%, Mute: "
             << (prev_mute_state_ ? "Muted" : "Unmuted") << ")");
    return true;
#else
    impl_->dummy_muted = prev_mute_state_;
    impl_->dummy_volume = prev_volume_level_;
    is_silenced_by_us_ = false;
    LOG_INFO("AudioEndpointControl [Mock]: PC Speaker restored to previous state");
    return true;
#endif
}

bool AudioEndpointControl::set_mute(bool mute) {
    std::lock_guard<std::mutex> lock(mutex_);
#if defined(_WIN32)
    if (!impl_->endpoint_volume) return false;
    return SUCCEEDED(impl_->endpoint_volume->SetMute(mute ? TRUE : FALSE, nullptr));
#elif defined(__linux__) && !defined(__ANDROID__)
    if (!impl_->elem || !impl_->has_switch) return false;
    return impl_->snd_mixer_selem_set_playback_switch_all(impl_->elem, mute ? 0 : 1) >= 0;
#else
    impl_->dummy_muted = mute;
    return true;
#endif
}

bool AudioEndpointControl::is_muted() const {
    std::lock_guard<std::mutex> lock(mutex_);
#if defined(_WIN32)
    if (!impl_->endpoint_volume) return false;
    BOOL muted = FALSE;
    impl_->endpoint_volume->GetMute(&muted);
    return muted == TRUE;
#elif defined(__linux__) && !defined(__ANDROID__)
    if (!impl_->elem || !impl_->has_switch) return false;
    impl_->snd_mixer_handle_events(impl_->mixer);
    int sw = 1;
    if (impl_->snd_mixer_selem_get_playback_switch(impl_->elem, kAlsaFrontLeft, &sw) == 0) {
        return sw == 0;
    }
    return false;
#else
    return impl_->dummy_muted;
#endif
}

bool AudioEndpointControl::set_volume(float volume_0_to_1) {
    std::lock_guard<std::mutex> lock(mutex_);
    float clamped = std::clamp(volume_0_to_1, 0.0f, 1.0f);
#if defined(_WIN32)
    if (!impl_->endpoint_volume) return false;
    return SUCCEEDED(impl_->endpoint_volume->SetMasterVolumeLevelScalar(clamped, nullptr));
#elif defined(__linux__) && !defined(__ANDROID__)
    if (!impl_->elem || !impl_->has_volume || impl_->vol_max <= impl_->vol_min) return false;
    long raw = impl_->vol_min + static_cast<long>(std::lround(
        clamped * static_cast<float>(impl_->vol_max - impl_->vol_min)));
    return impl_->snd_mixer_selem_set_playback_volume_all(impl_->elem, raw) >= 0;
#else
    impl_->dummy_volume = clamped;
    return true;
#endif
}

float AudioEndpointControl::get_volume() const {
    std::lock_guard<std::mutex> lock(mutex_);
#if defined(_WIN32)
    if (!impl_->endpoint_volume) return 1.0f;
    float vol = 1.0f;
    impl_->endpoint_volume->GetMasterVolumeLevelScalar(&vol);
    return vol;
#elif defined(__linux__) && !defined(__ANDROID__)
    if (!impl_->elem || !impl_->has_volume || impl_->vol_max <= impl_->vol_min) return 1.0f;
    impl_->snd_mixer_handle_events(impl_->mixer);
    long raw = impl_->vol_max;
    if (impl_->snd_mixer_selem_get_playback_volume(impl_->elem, kAlsaFrontLeft, &raw) == 0) {
        return static_cast<float>(raw - impl_->vol_min) /
               static_cast<float>(impl_->vol_max - impl_->vol_min);
    }
    return 1.0f;
#else
    return impl_->dummy_volume;
#endif
}

} // namespace audiorouter
