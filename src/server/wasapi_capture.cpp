#include "wasapi_capture.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#if defined(_WIN32)
    #include <windows.h>
    #include <mmdeviceapi.h>
    #include <audioclient.h>
    #include <avrt.h>
    #include <functiondiscoverykeys_devpkey.h>
    #pragma comment(lib, "avrt.lib")
#endif

namespace audiorouter {

struct WasapiCapture::Impl {
#if defined(_WIN32)
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audio_client = nullptr;
    IAudioCaptureClient* capture_client = nullptr;
    WAVEFORMATEX* wave_format = nullptr;
    HANDLE event_handle = nullptr;
    bool com_initialized = false;
    bool is_float = false;
    uint32_t device_channels = 2;
    uint32_t device_sample_rate = 48000;
    uint32_t device_bits_per_sample = 16;
#else
    bool dummy_running = false;
#endif
};

WasapiCapture::WasapiCapture()
    : impl_(std::make_unique<Impl>()),
      is_running_(false),
      device_name_("Default Windows Output Device") {}

WasapiCapture::~WasapiCapture() {
    stop();
}

bool WasapiCapture::start(const AudioConfig& desired_config, AudioConfig& actual_config) {
    if (is_running_) {
        actual_config = actual_config_;
        return true;
    }

#if defined(_WIN32)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        impl_->com_initialized = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        impl_->com_initialized = false;
    } else {
        LOG_ERROR("WasapiCapture: CoInitializeEx failed: 0x" << std::hex << hr);
        return false;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&impl_->enumerator);
    if (FAILED(hr) || !impl_->enumerator) {
        LOG_ERROR("WasapiCapture: CoCreateInstance MMDeviceEnumerator failed: 0x" << std::hex << hr);
        return false;
    }

    hr = impl_->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &impl_->device);
    if (FAILED(hr) || !impl_->device) {
        LOG_ERROR("WasapiCapture: GetDefaultAudioEndpoint failed: 0x" << std::hex << hr);
        return false;
    }

    // Retrieve friendly name
    IPropertyStore* p_props = nullptr;
    if (SUCCEEDED(impl_->device->OpenPropertyStore(STGM_READ, &p_props))) {
        PROPVARIANT var_name;
        PropVariantInit(&var_name);
        if (SUCCEEDED(p_props->GetValue(PKEY_Device_FriendlyName, &var_name))) {
            char name_buf[256] = {0};
            WideCharToMultiByte(CP_UTF8, 0, var_name.pwszVal, -1, name_buf, sizeof(name_buf), NULL, NULL);
            device_name_ = name_buf;
            PropVariantClear(&var_name);
        }
        p_props->Release();
    }

    hr = impl_->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&impl_->audio_client);
    if (FAILED(hr) || !impl_->audio_client) {
        LOG_ERROR("WasapiCapture: Activate IAudioClient failed: 0x" << std::hex << hr);
        return false;
    }

    hr = impl_->audio_client->GetMixFormat(&impl_->wave_format);
    if (FAILED(hr) || !impl_->wave_format) {
        LOG_ERROR("WasapiCapture: GetMixFormat failed: 0x" << std::hex << hr);
        return false;
    }

    // Parse mix format
    impl_->device_sample_rate = impl_->wave_format->nSamplesPerSec;
    impl_->device_channels = impl_->wave_format->nChannels;
    impl_->device_bits_per_sample = impl_->wave_format->wBitsPerSample;

    if (impl_->wave_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE* p_ex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(impl_->wave_format);
        if (IsEqualGUID(p_ex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            impl_->is_float = true;
        } else {
            impl_->is_float = false;
        }
    } else if (impl_->wave_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        impl_->is_float = true;
    } else {
        impl_->is_float = false;
    }

    LOG_INFO("WASAPI Device Mix Format: " << impl_->device_sample_rate << "Hz, "
             << impl_->device_channels << " channels, "
             << impl_->device_bits_per_sample << " bits, "
             << (impl_->is_float ? "IEEE Float" : "PCM Integer"));

    // Buffer duration: 50ms buffer (500,000 in 100ns units)
    REFERENCE_TIME hns_buffer_duration = 500000;
    hr = impl_->audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        hns_buffer_duration,
        0,
        impl_->wave_format,
        nullptr
    );

    if (FAILED(hr)) {
        LOG_ERROR("WasapiCapture: IAudioClient::Initialize failed: 0x" << std::hex << hr);
        return false;
    }

    hr = impl_->audio_client->GetService(__uuidof(IAudioCaptureClient), (void**)&impl_->capture_client);
    if (FAILED(hr) || !impl_->capture_client) {
        LOG_ERROR("WasapiCapture: GetService IAudioCaptureClient failed: 0x" << std::hex << hr);
        return false;
    }

    // Set actual stream config for client (stereo 16-bit PCM at device sample rate)
    actual_config_.sample_rate = impl_->device_sample_rate;
    actual_config_.channels = 2; // We downmix to stereo
    actual_config_.format = AudioSampleFormat::PCM_S16LE;
    actual_config_.frames_per_packet = desired_config.frames_per_packet > 0 ? desired_config.frames_per_packet : 480;

    actual_config = actual_config_;

    hr = impl_->audio_client->Start();
    if (FAILED(hr)) {
        LOG_ERROR("WasapiCapture: IAudioClient::Start failed: 0x" << std::hex << hr);
        return false;
    }

    is_running_ = true;
    capture_thread_ = std::thread(&WasapiCapture::capture_thread_func, this);

    LOG_INFO("WasapiCapture: Loopback capture active on '" << device_name_ << "'");
    return true;
#else
    LOG_INFO("WasapiCapture: Mock capture started (Non-Windows platform)");
    actual_config_ = desired_config;
    actual_config_.sample_rate = desired_config.sample_rate > 0 ? desired_config.sample_rate : 48000;
    actual_config_.channels = 2;
    actual_config_.format = AudioSampleFormat::PCM_S16LE;
    actual_config_.frames_per_packet = desired_config.frames_per_packet > 0 ? desired_config.frames_per_packet : 480;
    actual_config = actual_config_;

    is_running_ = true;
    capture_thread_ = std::thread(&WasapiCapture::capture_thread_func, this);
    return true;
#endif
}

void WasapiCapture::stop() {
    if (!is_running_) return;

    is_running_ = false;
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }

#if defined(_WIN32)
    if (impl_->audio_client) {
        impl_->audio_client->Stop();
    }
    if (impl_->capture_client) {
        impl_->capture_client->Release();
        impl_->capture_client = nullptr;
    }
    if (impl_->wave_format) {
        CoTaskMemFree(impl_->wave_format);
        impl_->wave_format = nullptr;
    }
    if (impl_->audio_client) {
        impl_->audio_client->Release();
        impl_->audio_client = nullptr;
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
    LOG_INFO("WasapiCapture: Loopback capture stopped");
}

bool WasapiCapture::is_capturing() const {
    return is_running_;
}

void WasapiCapture::set_audio_callback(AudioCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_ = std::move(cb);
}

std::string WasapiCapture::get_device_name() const {
    return device_name_;
}

void WasapiCapture::capture_thread_func() {
#if defined(_WIN32)
    DWORD task_index = 0;
    HANDLE h_task = AvSetMmThreadCharacteristicsA("Audio", &task_index);
    if (!h_task) {
        LOG_WARN("WasapiCapture: AvSetMmThreadCharacteristics failed");
    }

    std::vector<float> temp_stereo_float;
    std::vector<int16_t> output_pcm_s16;

    while (is_running_) {
        UINT32 packet_length = 0;
        HRESULT hr = impl_->capture_client->GetNextPacketSize(&packet_length);
        if (FAILED(hr)) {
            sleep_ms(5);
            continue;
        }

        if (packet_length == 0) {
            // No audio packet currently ready; sleep briefly (e.g. 3ms) to prevent busy-spin
            sleep_ms(3);
            continue;
        }

        while (packet_length > 0 && is_running_) {
            BYTE* p_data = nullptr;
            UINT32 num_frames_available = 0;
            DWORD flags = 0;

            hr = impl_->capture_client->GetBuffer(&p_data, &num_frames_available, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                break;
            }

            if (num_frames_available > 0) {
                temp_stereo_float.resize(num_frames_available * 2);
                output_pcm_s16.resize(num_frames_available * 2);

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(output_pcm_s16.begin(), output_pcm_s16.end(), 0);
                } else if (p_data != nullptr) {
                    if (impl_->is_float) {
                        const float* float_data = reinterpret_cast<const float*>(p_data);
                        if (impl_->device_channels == 2) {
                            AudioConverter::float32_to_s16le(float_data, output_pcm_s16.data(), num_frames_available * 2);
                        } else {
                            AudioConverter::downmix_to_stereo_float(float_data, impl_->device_channels,
                                                                    temp_stereo_float.data(), num_frames_available);
                            AudioConverter::float32_to_s16le(temp_stereo_float.data(), output_pcm_s16.data(),
                                                             num_frames_available * 2);
                        }
                    } else if (impl_->device_bits_per_sample == 16) {
                        const int16_t* int16_data = reinterpret_cast<const int16_t*>(p_data);
                        if (impl_->device_channels == 2) {
                            std::copy(int16_data, int16_data + num_frames_available * 2, output_pcm_s16.data());
                        } else {
                            // Downmix int16 multi-channel to stereo
                            for (size_t i = 0; i < num_frames_available; ++i) {
                                output_pcm_s16[i * 2 + 0] = int16_data[i * impl_->device_channels + 0];
                                output_pcm_s16[i * 2 + 1] = int16_data[i * impl_->device_channels + 1];
                            }
                        }
                    }
                }

                // Send to audio callback
                {
                    std::lock_guard<std::mutex> lock(callback_mutex_);
                    if (callback_) {
                        callback_(output_pcm_s16.data(), num_frames_available, actual_config_);
                    }
                }
            }

            impl_->capture_client->ReleaseBuffer(num_frames_available);
            hr = impl_->capture_client->GetNextPacketSize(&packet_length);
            if (FAILED(hr)) break;
        }
    }

    if (h_task) {
        AvRevertMmThreadCharacteristics(h_task);
    }
#else
    // Linux/POSIX Mock Capture Thread: Generates low-amplitude test sine wave / clock
    double phase = 0.0;
    const double freq = 440.0; // 440 Hz standard A
    const double rate = static_cast<double>(actual_config_.sample_rate);
    const size_t frames_per_block = actual_config_.frames_per_packet;
    std::vector<int16_t> mock_buf(frames_per_block * 2);

    while (is_running_) {
        uint64_t start_time = get_time_us();

        for (size_t i = 0; i < frames_per_block; ++i) {
            double sample = std::sin(phase) * 0.25; // 25% volume test tone
            phase += 2.0 * 3.14159265358979323846 * freq / rate;
            if (phase > 2.0 * 3.14159265358979323846) {
                phase -= 2.0 * 3.14159265358979323846;
            }
            int16_t s16 = static_cast<int16_t>(sample * 32767.0);
            mock_buf[i * 2 + 0] = s16;
            mock_buf[i * 2 + 1] = s16;
        }

        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (callback_) {
                callback_(mock_buf.data(), frames_per_block, actual_config_);
            }
        }

        uint64_t elapsed_us = get_time_us() - start_time;
        uint64_t target_us = (static_cast<uint64_t>(frames_per_block) * 1000000ULL) / actual_config_.sample_rate;
        if (target_us > elapsed_us) {
            sleep_us(static_cast<uint32_t>(target_us - elapsed_us));
        }
    }
#endif
}

} // namespace audiorouter
