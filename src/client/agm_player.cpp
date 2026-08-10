#include "agm_player.hpp"
#include "android_helpers.hpp"
#include "../common/logger.hpp"

#include <dlfcn.h>
#include <cstring>
#include <algorithm>
#include <vector>

// ABI of external/tinyalsa as seen by the vendor plugin: pcm_config is read by
// field offset, so the full layout must be declared (not a minimal subset).
namespace {

enum pcm_format {
    PCM_FORMAT_S16_LE = 0,
};

struct pcm_config {
    unsigned int channels;
    unsigned int rate;
    unsigned int period_size;
    unsigned int period_count;
    unsigned int format;  // enum pcm_format
    unsigned int start_threshold;
    unsigned int stop_threshold;
    unsigned int silence_threshold;
    unsigned int silence_size;
    unsigned int avail_min;
    unsigned int prealloc_size;
    unsigned int prealloc_count;
    unsigned int mmap_playback_start;
    unsigned int mmap_capture_start;
    unsigned int mmap_playback_avail_needs_fill;
    unsigned int mmap_capture_avail_needs_fill;
};

typedef struct pcm* (*pcm_open_fn)(unsigned int card, unsigned int device, unsigned int flags,
                                   const struct pcm_config* config);
typedef int (*pcm_close_fn)(struct pcm* pcm);
typedef int (*pcm_write_fn)(struct pcm* pcm, const void* data, unsigned int count);
typedef int (*pcm_is_ready_fn)(const struct pcm* pcm);
typedef const char* (*pcm_get_error_fn)(const struct pcm* pcm);
typedef int (*pcm_drain_fn)(struct pcm* pcm);
typedef int (*pcm_drop_fn)(struct pcm* pcm);

typedef struct mixer* (*mixer_open_fn)(unsigned int card);
typedef void (*mixer_close_fn)(struct mixer* mixer);
struct mixer_ctl;
typedef struct mixer_ctl* (*mixer_get_ctl_fn)(struct mixer* mixer, const char* name);
typedef int (*mixer_ctl_set_value_fn)(struct mixer_ctl* ctl, unsigned int id, int value);

constexpr unsigned int kPcmOut = 0x00000000;

// Card/device pair used by the vendor's own agmplay tool for the speaker
// backend "CODEC_DMA-LPAIF_RXTX-RX-1".
constexpr unsigned int kAgmCard = 100;
constexpr unsigned int kAgmDevice = 100;

} // namespace

namespace audiorouter {

struct AgmPlayer::PcmApi {
    void* tinyalsa_handle = nullptr;
    pcm_open_fn pcm_open = nullptr;
    pcm_close_fn pcm_close = nullptr;
    pcm_write_fn pcm_write = nullptr;
    pcm_is_ready_fn pcm_is_ready = nullptr;
    pcm_get_error_fn pcm_get_error = nullptr;
    pcm_drain_fn pcm_drain = nullptr;
    pcm_drop_fn pcm_drop = nullptr;
    mixer_open_fn mixer_open = nullptr;
    mixer_close_fn mixer_close = nullptr;
    mixer_get_ctl_fn mixer_get_ctl = nullptr;
    mixer_ctl_set_value_fn mixer_ctl_set_value = nullptr;
};

AgmPlayer::AgmPlayer() : api_(nullptr), pcm_impl_(nullptr), mixer_impl_(nullptr), is_open_(false) {}

AgmPlayer::~AgmPlayer() {
    close();
    unload_tinyalsa();
}

bool AgmPlayer::load_tinyalsa() {
    if (api_) return true;
    if (api_) {
        delete api_;
        api_ = nullptr;
    }

    const char* tinyalsa_paths[] = {
        "/vendor/lib64/libtinyalsa.so",
        "/vendor/lib/libtinyalsa.so",
        "/system/lib64/libtinyalsa.so",
        "/system/lib/libtinyalsa.so",
        "libtinyalsa.so",
    };

    auto* api = new PcmApi();
    for (const char* path : tinyalsa_paths) {
        api->tinyalsa_handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (api->tinyalsa_handle) break;
    }
    if (!api->tinyalsa_handle) {
        LOG_ERROR("AgmPlayer: could not dlopen libtinyalsa.so: " << dlerror());
        LOG_ERROR("AgmPlayer: run this binary via 'su' (root); if the linker namespace blocks vendor paths, "
                  << "launch with: su -c \"LD_LIBRARY_PATH=/vendor/lib64 ./audiorouter_client -s <PC_IP> -d agm\"");
        delete api;
        return false;
    }

    api->pcm_open = reinterpret_cast<pcm_open_fn>(dlsym(api->tinyalsa_handle, "pcm_open"));
    api->pcm_close = reinterpret_cast<pcm_close_fn>(dlsym(api->tinyalsa_handle, "pcm_close"));
    api->pcm_write = reinterpret_cast<pcm_write_fn>(dlsym(api->tinyalsa_handle, "pcm_write"));
    api->pcm_is_ready = reinterpret_cast<pcm_is_ready_fn>(dlsym(api->tinyalsa_handle, "pcm_is_ready"));
    api->pcm_get_error = reinterpret_cast<pcm_get_error_fn>(dlsym(api->tinyalsa_handle, "pcm_get_error"));
    api->pcm_drain = reinterpret_cast<pcm_drain_fn>(dlsym(api->tinyalsa_handle, "pcm_drain"));
    api->pcm_drop = reinterpret_cast<pcm_drop_fn>(dlsym(api->tinyalsa_handle, "pcm_drop"));
    api->mixer_open = reinterpret_cast<mixer_open_fn>(dlsym(api->tinyalsa_handle, "mixer_open"));
    api->mixer_close = reinterpret_cast<mixer_close_fn>(dlsym(api->tinyalsa_handle, "mixer_close"));
    api->mixer_get_ctl = reinterpret_cast<mixer_get_ctl_fn>(dlsym(api->tinyalsa_handle, "mixer_get_ctl"));
    api->mixer_ctl_set_value = reinterpret_cast<mixer_ctl_set_value_fn>(dlsym(api->tinyalsa_handle, "mixer_ctl_set_value"));

    if (!api->pcm_open || !api->pcm_write || !api->pcm_close || !api->mixer_open || !api->mixer_close ||
        !api->mixer_get_ctl || !api->mixer_ctl_set_value) {
        LOG_ERROR("AgmPlayer: libtinyalsa.so is missing required PCM symbols: " << dlerror());
        dlclose(api->tinyalsa_handle);
        delete api;
        return false;
    }

    api_ = api;
    return true;
}

void AgmPlayer::unload_tinyalsa() {
    if (api_) {
        if (api_->tinyalsa_handle) dlclose(api_->tinyalsa_handle);
        delete api_;
        api_ = nullptr;
    }
}

bool AgmPlayer::open(const AudioConfig& config, const std::string& device_name) {
#if defined(__linux__) || defined(__ANDROID__)
    if (is_open_) close();

    config_ = config;
    backend_ = "CODEC_DMA-LPAIF_RXTX-RX-1";
    const std::string name = device_name.empty() ? "agm" : device_name;
    if (name.rfind("agm:", 0) == 0) {
        std::string rest = name.substr(4);
        if (!rest.empty()) backend_ = rest;
    }

    if (!load_tinyalsa()) return false;

    // Route the codec to the speaker before opening the PCM. Best effort: if
    // audioserver re-routes the mixer this is redone on the next open.
    AndroidHelpers::apply_speaker_routing();

    struct pcm_config cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.channels = 1;  // mono speaker backend
    cfg.rate = config.sample_rate > 0 ? static_cast<unsigned int>(config.sample_rate) : 48000;
    cfg.period_size = 240;  // 5 ms @ 48 kHz
    cfg.period_count = 4;
    cfg.format = PCM_FORMAT_S16_LE;
    cfg.start_threshold = cfg.period_size;
    cfg.stop_threshold = cfg.period_size * cfg.period_count;
    cfg.silence_threshold = 0;
    cfg.silence_size = 0;
    cfg.avail_min = cfg.period_size;

    LOG_INFO("AgmPlayer: opening AGM mixer plugin on card " << kAgmCard << "...");
    mixer_impl_ = api_->mixer_open(kAgmCard);
    if (!mixer_impl_) {
        LOG_ERROR("AgmPlayer: mixer_open(" << kAgmCard
                  << ") failed - libagm_pcm_plugin.so requires the card-100 mixer context (GKV) to be "
                  << "registered before pcm_open. Is the AGM mixer plugin present?");
        return false;
    }

    // Register the backend graph key vectors (GKV) that the AGM PCM plugin
    // needs: the "<backend> rate ch fmt" mixer control carries the backend's
    // rate / channels / bit-width. agmplay sets this before pcm_open; without
    // it the plugin aborts with "failed to open plugin" (-32).
    const std::string rate_ctl_name = backend_ + " rate ch fmt";
    struct mixer_ctl* rate_ctl = api_->mixer_get_ctl(mixer_impl_, rate_ctl_name.c_str());
    if (rate_ctl) {
        const int set_ret = api_->mixer_ctl_set_value(rate_ctl, 0, static_cast<int>(cfg.rate));
        api_->mixer_ctl_set_value(rate_ctl, 1, 1);   // mono
        api_->mixer_ctl_set_value(rate_ctl, 2, 16);  // S16_LE
        LOG_INFO("AgmPlayer: set backend control '" << rate_ctl_name << "' = " << cfg.rate
                 << " Hz, 1 ch, 16 bit (rc " << set_ret << ")");
    } else {
        LOG_WARN("AgmPlayer: backend control '" << rate_ctl_name << "' not found on card " << kAgmCard
                 << "; the AGM PCM plugin may refuse to open. List controls with: "
                 << "tinymix -D " << kAgmCard);
    }

    LOG_INFO("AgmPlayer: opening AGM PCM card " << kAgmCard << " device " << kAgmDevice
             << " (backend '" << backend_ << "', " << cfg.rate << " Hz mono S16_LE)...");

    pcm_impl_ = api_->pcm_open(kAgmCard, kAgmDevice, kPcmOut, &cfg);
    if (!pcm_impl_ || (api_->pcm_is_ready && !api_->pcm_is_ready(pcm_impl_))) {
        const char* err = (pcm_impl_ && api_->pcm_get_error) ? api_->pcm_get_error(pcm_impl_)
                                                             : "pcm_open returned null";
        LOG_ERROR("AgmPlayer: pcm_open(100, 100) failed: " << err);
        if (pcm_impl_) {
            api_->pcm_close(pcm_impl_);
            pcm_impl_ = nullptr;
        }
        api_->mixer_close(mixer_impl_);
        mixer_impl_ = nullptr;
        return false;
    }

    is_open_ = true;
    LOG_INFO("AgmPlayer: AGM PCM open complete on backend '" << backend_ << "' (" << config.to_string()
             << ", mono output)");
    return true;
#else
    (void)config;
    (void)device_name;
    LOG_INFO("AgmPlayer: AGM playback not available on this platform");
    return false;
#endif
}

void AgmPlayer::close() {
#if defined(__linux__) || defined(__ANDROID__)
    is_open_ = false;
    if (api_) {
        if (pcm_impl_) {
            api_->pcm_close(pcm_impl_);
            pcm_impl_ = nullptr;
        }
        if (mixer_impl_) {
            api_->mixer_close(mixer_impl_);
            mixer_impl_ = nullptr;
        }
        LOG_INFO("AgmPlayer: AGM PCM + mixer closed");
    }
#endif
}

bool AgmPlayer::is_open() const {
    return is_open_;
}

size_t AgmPlayer::write_frames(const void* pcm_data, size_t num_frames) {
    if (!is_open_ || !pcm_data || num_frames == 0) return 0;
#if defined(__linux__) || defined(__ANDROID__)
    if (!api_ || !pcm_impl_) return 0;

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

    size_t bytes = num_frames * 2;  // mono S16
    int rc = api_->pcm_write(pcm_impl_, src, static_cast<unsigned int>(bytes));
    if (rc != 0) {
        LOG_DEBUG("AgmPlayer: pcm_write failed: "
                  << (api_->pcm_get_error ? api_->pcm_get_error(pcm_impl_) : "pcm error") << " (rc " << rc << ")");
        // Drop the current buffer content and retry once to recover from XRUN.
        if (api_->pcm_drop) api_->pcm_drop(pcm_impl_);
        rc = api_->pcm_write(pcm_impl_, src, static_cast<unsigned int>(bytes));
        if (rc != 0) {
            return 0;
        }
        LOG_DEBUG("AgmPlayer: pcm_write recovered after drop");
    }

    return num_frames;
#else
    (void)pcm_data;
    (void)num_frames;
    return 0;
#endif
}

size_t AgmPlayer::get_buffer_delay_frames() const {
    // The vendor PCM plugin does not expose a stable delay query; report none.
    return 0;
}

void AgmPlayer::flush() {
#if defined(__linux__) || defined(__ANDROID__)
    if (api_ && pcm_impl_ && api_->pcm_drain) {
        api_->pcm_drain(pcm_impl_);
    }
#endif
}

std::string AgmPlayer::get_device_name() const {
    return "agm:" + backend_;
}

} // namespace audiorouter