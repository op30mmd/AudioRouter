#include "audio_endpoint_control.hpp"
#include "../common/logger.hpp"
#include <algorithm>

#if defined(_WIN32)
    #include <windows.h>
    #include <mmdeviceapi.h>
    #include <endpointvolume.h>
#endif

namespace audiorouter {

struct AudioEndpointControl::Impl {
#if defined(_WIN32)
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioEndpointVolume* endpoint_volume = nullptr;
    bool com_initialized = false;
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
#else
    LOG_INFO("AudioEndpointControl: Mock endpoint volume controller initialized (Non-Windows)");
    return true;
#endif
}

void AudioEndpointControl::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_silenced_by_us_) {
        // Unmute before releasing COM pointers
        if (active_mute_method_ == MuteMethod::EndpointMute || active_mute_method_ == MuteMethod::Both) {
#if defined(_WIN32)
            if (impl_->endpoint_volume) {
                impl_->endpoint_volume->SetMute(prev_mute_state_ ? TRUE : FALSE, nullptr);
            }
#endif
        }
        if (active_mute_method_ == MuteMethod::VolumeZero || active_mute_method_ == MuteMethod::Both) {
#if defined(_WIN32)
            if (impl_->endpoint_volume) {
                impl_->endpoint_volume->SetMasterVolumeLevelScalar(prev_volume_level_, nullptr);
            }
#endif
        }
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
#else
    return impl_->dummy_volume;
#endif
}

} // namespace audiorouter
