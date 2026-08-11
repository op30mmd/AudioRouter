#include "agm_player.hpp"
#include "android_helpers.hpp"
#include "../common/logger.hpp"

#include <dlfcn.h>
#include <cstring>
#include <cstdlib>
#include <cctype>
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
// NOTE: in tinyalsa (both AOSP and qcom's fork) mixer_get_ctl() is indexed by
// unsigned id; the by-name API is the separate mixer_get_ctl_by_name() symbol.
typedef struct mixer_ctl* (*mixer_get_ctl_fn)(struct mixer* mixer, unsigned int id);
typedef struct mixer_ctl* (*mixer_get_ctl_by_name_fn)(struct mixer* mixer, const char* name);
typedef unsigned int (*mixer_get_num_ctls_fn)(struct mixer* mixer);
typedef const char* (*mixer_ctl_get_name_fn)(const struct mixer_ctl* ctl);
typedef int (*mixer_ctl_set_value_fn)(struct mixer_ctl* ctl, unsigned int id, int value);
typedef int (*mixer_ctl_set_enum_by_string_fn)(struct mixer_ctl* ctl, const char* string);
typedef int (*mixer_ctl_set_array_fn)(struct mixer_ctl* ctl, const void* data, size_t num_bytes);

// AudioReach graph/calibration key values (kvh2xml.h). These are the keys the
// A05s's ACDB uses to identify the speaker playback graph; agmplay's metadata
// writes on the device resolve to exactly these pairs
// (gkv[0] = 0xa1000000/0xa100000e PCM_LL_PLAYBACK, INSTANCE, gkv[2] =
// 0xac000000/0xac000002 DEVICEPP_RX_AUDIO_MBDRC).
constexpr uint32_t kKvStreamRx        = 0xA1000000;
constexpr uint32_t kKvPcmLlPlayback   = 0xA100000E;
constexpr uint32_t kKvDeviceRx        = 0xA2000000;
constexpr uint32_t kKvSpeaker         = 0xA2000001;
constexpr uint32_t kKvVolume          = 0xA4000000;
constexpr uint32_t kKvSamplingRate    = 0xA5000000;
constexpr uint32_t kKvBitWidth        = 0xA6000000;
constexpr uint32_t kKvInstance        = 0xAB000000;
constexpr uint32_t kKvInstance1       = 0x1;
constexpr uint32_t kKvDevicePpRx      = 0xAC000000;
constexpr uint32_t kKvDevicePpRxMbDrc = 0xAC000002;

// SNDRV_PCM_FORMAT_S16_LE (the plugin maps this via alsa_to_agm_fmt, so the
// raw bits value 16 used before produced AGM_FORMAT_INVALID on the device).
constexpr uint32_t kAlsaFormatS16Le = 2;
// AGM_DATA_FORMAT_FIXED_POINT (0 = AGM_DATA_FORMAT_INVALID).
constexpr uint32_t kAgmDataFormatFixedPoint = 1;

void AppendU32Le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

// "<backend> metadata" control payload (same layout as
// set_agm_audio_intf_metadata in the AGM test apps):
//   uint32 num_gkv, agm_key_value gkv[], uint32 num_ckv,
//   agm_key_value ckv[], prop_data {prop_id, num_values, values[]}
std::vector<uint8_t> BuildBackendMetadataPayload(uint32_t rate, uint32_t bit_width) {
    std::vector<uint8_t> m;
    AppendU32Le(m, 1);               // num_gkv
    AppendU32Le(m, kKvDeviceRx);     // gkv[0].key  = DEVICERX
    AppendU32Le(m, kKvSpeaker);      // gkv[0].value = SPEAKER
    AppendU32Le(m, 2);               // num_ckv
    AppendU32Le(m, kKvSamplingRate); // ckv[0].key = SAMPLINGRATE
    AppendU32Le(m, rate);
    AppendU32Le(m, kKvBitWidth);     // ckv[1].key = BITWIDTH
    AppendU32Le(m, bit_width);
    AppendU32Le(m, 0);               // prop_id
    AppendU32Le(m, 0);               // num_values
    return m;
}

// "PCM<dev> metadata" control payload (same layout as set_agm_stream_metadata
// for PLAYBACK with an instance key): STREAMRX:PCM_LL_PLAYBACK +
// INSTANCE:INSTANCE_1, ckv VOLUME:LEVEL_0. The A05s's agmplay additionally
// carries the DEVICEPP_RX key in the stream metadata (its log shows
// gkv[2] = 0xac000000/0xac000002); without that pair the ADSP finds no graph
// for the tag (AR_ENOTEXIST -> -ENODEV), so it is included by default.
// AUDIOROUTER_AGM_DEVICEPP_KV=0 disables it.
std::vector<uint8_t> BuildStreamMetadataPayload(bool include_devicepp) {
    std::vector<uint8_t> m;
    AppendU32Le(m, include_devicepp ? 3u : 2u);  // num_gkv
    AppendU32Le(m, kKvStreamRx);                 // gkv[0].key = STREAMRX
    AppendU32Le(m, kKvPcmLlPlayback);            // gkv[0].value = PCM_LL_PLAYBACK
    AppendU32Le(m, kKvInstance);                 // gkv[1].key = INSTANCE
    AppendU32Le(m, kKvInstance1);                // gkv[1].value = INSTANCE_1
    if (include_devicepp) {
        AppendU32Le(m, kKvDevicePpRx);           // gkv[2].key = DEVICEPP_RX
        AppendU32Le(m, kKvDevicePpRxMbDrc);      // gkv[2].value = DEVICEPP_RX_AUDIO_MBDRC
    }
    AppendU32Le(m, 1);                           // num_ckv
    AppendU32Le(m, kKvVolume);                   // ckv[0].key = VOLUME
    AppendU32Le(m, 0);                           // ckv[0].value = LEVEL_0
    AppendU32Le(m, 0);                           // prop_id
    AppendU32Le(m, 0);                           // num_values
    return m;
}

// AGM media config, matching agm_api.h field order exactly (the library
// reads it by offset, so the layout must be byte-for-byte correct):
//   rate, channels, format, data_format
// format is the tinyalsa-style enum: 0 = PCM S16_LE.
struct agm_media_config {
    uint32_t rate;         // sample rate in Hz
    uint32_t channels;
    uint32_t format;       // 0 = S16_LE (tinyalsa enum)
    uint32_t data_format;  // 0 = default / fixed-point
};
typedef int (*agm_aif_set_media_config_fn)(const char* aif_name, struct agm_media_config* media_config);

constexpr unsigned int kPcmOut = 0x00000000;

// Card/device pair used by the vendor's own agmplay tool for the speaker
// backend "CODEC_DMA-LPAIF_RXTX-RX-1".
constexpr unsigned int kAgmCard = 100;
constexpr unsigned int kAgmDevice = 100;

// Fallback control IDs verified on the A05s (tinymix -D 100):
//   #9  = "<backend> rate"      #51 = "PCM100 connect"
// Used only if name lookup yields nothing.
constexpr unsigned int kRateCtlId = 9;
constexpr unsigned int kConnectCtlId = 51;

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
    mixer_get_ctl_by_name_fn mixer_get_ctl_by_name = nullptr;
    mixer_get_num_ctls_fn mixer_get_num_ctls = nullptr;
    mixer_ctl_get_name_fn mixer_ctl_get_name = nullptr;
    mixer_ctl_set_value_fn mixer_ctl_set_value = nullptr;
    mixer_ctl_set_enum_by_string_fn mixer_ctl_set_enum_by_string = nullptr;
    mixer_ctl_set_array_fn mixer_ctl_set_array = nullptr;
    // Best-effort AGM API (libagmclient): media config on the AIF prior to open.
    void* agm_handle = nullptr;
    agm_aif_set_media_config_fn agm_aif_set_media_config = nullptr;
};

// Result of a mixer control lookup: the resolved handle plus how it was found,
// so the open() log shows exactly which path succeeded.
struct CtlLookup {
    struct mixer_ctl* ctl = nullptr;
    std::string resolved = "?";
    std::string how = "not found";
};

bool NameEqualsIgnoreCase(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        const char la = static_cast<char>(std::tolower(static_cast<unsigned char>(*a)));
        const char lb = static_cast<char>(std::tolower(static_cast<unsigned char>(*b)));
        if (la != lb) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

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
    api->mixer_get_ctl_by_name =
        reinterpret_cast<mixer_get_ctl_by_name_fn>(dlsym(api->tinyalsa_handle, "mixer_get_ctl_by_name"));
    api->mixer_get_num_ctls =
        reinterpret_cast<mixer_get_num_ctls_fn>(dlsym(api->tinyalsa_handle, "mixer_get_num_ctls"));
    api->mixer_ctl_get_name = reinterpret_cast<mixer_ctl_get_name_fn>(dlsym(api->tinyalsa_handle, "mixer_ctl_get_name"));
    api->mixer_ctl_set_value = reinterpret_cast<mixer_ctl_set_value_fn>(dlsym(api->tinyalsa_handle, "mixer_ctl_set_value"));
    api->mixer_ctl_set_enum_by_string = reinterpret_cast<mixer_ctl_set_enum_by_string_fn>(dlsym(api->tinyalsa_handle, "mixer_ctl_set_enum_by_string"));
    api->mixer_ctl_set_array = reinterpret_cast<mixer_ctl_set_array_fn>(dlsym(api->tinyalsa_handle, "mixer_ctl_set_array"));
    api->agm_handle = dlopen("/vendor/lib64/libagmclient.so", RTLD_NOW | RTLD_GLOBAL);
    if (!api->agm_handle) api->agm_handle = dlopen("libagmclient.so", RTLD_NOW | RTLD_GLOBAL);
    if (api->agm_handle) {
        api->agm_aif_set_media_config =
            reinterpret_cast<agm_aif_set_media_config_fn>(dlsym(api->agm_handle, "agm_aif_set_media_config"));
        if (!api->agm_aif_set_media_config) LOG_DEBUG("AgmPlayer: libagmclient.so has no agm_aif_set_media_config");
    } else {
        LOG_DEBUG("AgmPlayer: libagmclient.so not found; skipping AIF media config (rc from dlopen: " << dlerror() << ")");
    }

    if (!api->pcm_open || !api->pcm_write || !api->pcm_close || !api->mixer_open || !api->mixer_close ||
        !api->mixer_get_ctl || !api->mixer_ctl_set_value || !api->mixer_ctl_set_enum_by_string) {
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
        // Intentionally do NOT dlclose() the vendor libraries. They register
        // pthread TLS destructors (libagm_mixer_plugin/libagmclient keep their
        // GKV state in thread-local storage); dlclose'ing them while the
        // audio-open retry thread may still exit triggers SIGSEGV inside
        // pthread_key_clean_all() (observed: 5-frame tombstone,
        // #00 <unknown> TLS destructor, #01 pthread_key_clean_all). Leaking
        // the handles until process exit is the safe pattern here.
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
    // Buffer geometry mirrors the vendor's agmplay (period 1024, 4 periods,
    // zero thresholds). The AGM session configures the ADSP graph's
    // shared-memory data endpoint from these values (gsl configure
    // read/write params); a smaller period/ring (e.g. 240/960 with nonzero
    // thresholds) opens fine but the graph never pulls data and pcm_write
    // blocks forever (observed: Played stuck at 960 frames, no sound).
    cfg.period_size = 1024;
    cfg.period_count = 4;
    cfg.format = PCM_FORMAT_S16_LE;
    cfg.start_threshold = 0;
    cfg.stop_threshold = 0;
    cfg.silence_threshold = 0;
    cfg.silence_size = 0;
    cfg.avail_min = 0;

    LOG_INFO("AgmPlayer: opening AGM mixer plugin on card " << kAgmCard << "...");
    mixer_impl_ = api_->mixer_open(kAgmCard);
    if (!mixer_impl_) {
        LOG_ERROR("AgmPlayer: mixer_open(" << kAgmCard
                  << ") failed - libagm_pcm_plugin.so requires the card-100 mixer context (GKV) to be "
                  << "registered before pcm_open. Is the AGM mixer plugin present?");
        return false;
    }

    if (api_->mixer_get_num_ctls && api_->mixer_get_ctl && api_->mixer_ctl_get_name) {
        const unsigned int num = api_->mixer_get_num_ctls(mixer_impl_);
        LOG_DEBUG("AgmPlayer: card " << kAgmCard << " exposes " << num << " mixer controls:");
        for (unsigned int id = 0; id < num; ++id) {
            struct mixer_ctl* c = api_->mixer_get_ctl(mixer_impl_, id);
            const char* n = c ? api_->mixer_ctl_get_name(c) : nullptr;
            LOG_DEBUG("AgmPlayer:   ctl[" << id << "] '" << (n ? n : "?") << "'");
        }
    }

    // Resolve a control by name. qcom's tinyalsa exposes mixer_get_ctl() by
    // index and the separate mixer_get_ctl_by_name(); on builds without the
    // by-name symbol, fall back to an index walk comparing names (exact, then
    // case-insensitive) so control lookup still works.
    auto find_ctl = [this](const std::string& ctl_name) -> CtlLookup {
        CtlLookup out;
        if (!mixer_impl_) return out;
        if (api_->mixer_get_ctl_by_name) {
            out.ctl = api_->mixer_get_ctl_by_name(mixer_impl_, ctl_name.c_str());
            out.how = "name '" + ctl_name + "'";
            if (out.ctl) {
                if (api_->mixer_ctl_get_name) {
                    const char* n = api_->mixer_ctl_get_name(out.ctl);
                    if (n) out.resolved = n;
                }
                return out;
            }
        }
        if (api_->mixer_get_ctl && api_->mixer_ctl_get_name && api_->mixer_get_num_ctls) {
            const unsigned int num = api_->mixer_get_num_ctls(mixer_impl_);
            for (unsigned int id = 0; id < num; ++id) {
                struct mixer_ctl* c = api_->mixer_get_ctl(mixer_impl_, id);
                if (!c) continue;
                const char* n = api_->mixer_ctl_get_name(c);
                if (!n) continue;
                if (std::strcmp(n, ctl_name.c_str()) == 0) {
                    out.ctl = c;
                    out.resolved = n;
                    out.how = "index " + std::to_string(id);
                    return out;
                }
            }
            for (unsigned int id = 0; id < num; ++id) {
                struct mixer_ctl* c = api_->mixer_get_ctl(mixer_impl_, id);
                if (!c) continue;
                const char* n = api_->mixer_ctl_get_name(c);
                if (!n) continue;
                if (NameEqualsIgnoreCase(n, ctl_name.c_str())) {
                    out.ctl = c;
                    out.resolved = n;
                    out.how = "index " + std::to_string(id) + " (case-insensitive)";
                    return out;
                }
            }
        }
        return out;
    };

    // Register the backend graph keys (GKV) the AGM PCM plugin needs, exactly
    // as agmplay does on this device:
    //   1) "<backend> rate ch fmt" - integer control carrying
    //      rate/ch/<alsa format enum>/<data format>. Format must be the ALSA
    //      enum value (2 = SNDRV_PCM_FORMAT_S16_LE), not the bit width, and
    //      data format must be 1 (AGM_DATA_FORMAT_FIXED_POINT); otherwise the
    //      AGM media config stored on the device is invalid and graph open
    //      fails with -EIO. (Verified on the A05s as control #9.)
    //   2) "<backend> metadata" - byte control registering the backend
    //      (DEVICERX/SPEAKER + SAMPLINGRATE/BITWIDTH) sub-graph KV pairs.
    //   3) "PCM<dev> control" = ZERO + "PCM<dev> metadata" - byte control
    //      registering the stream (STREAMRX/PCM_LL_PLAYBACK + INSTANCE) and
    //      stream-PP sub-graph KV pairs. Without these the session's merged
    //      GKV is empty, gsl_get_tags_with_module_info() finds no graph, and
    //      pcm_open fails with -5 (EIO).
    //   4) "PCM<dev> connect" - enum control routing PCM session <dev> onto
    //      the backend graph (verified as control #51). Without it the plugin
    //      sees an unrouted session and pcm_open fails with -32.
    // Successful tinymix settings from a shell do not travel into this
    // process, so all controls are set here, in-process, on our own mixer
    // handle.
    // --- 1) backend rate control: by name, then by known control ID ---
    std::string rate_ctl_name = backend_ + " rate";
    CtlLookup rate = find_ctl(rate_ctl_name);
    if (!rate.ctl) {
        rate_ctl_name = backend_ + " rate ch fmt";  // older AGM builds
        rate = find_ctl(rate_ctl_name);
    }
    if (!rate.ctl && api_->mixer_get_ctl) {
        rate.ctl = api_->mixer_get_ctl(mixer_impl_, kRateCtlId);
        rate.how = "id " + std::to_string(kRateCtlId);
        if (rate.ctl && api_->mixer_ctl_get_name) {
            const char* n = api_->mixer_ctl_get_name(rate.ctl);
            if (n) rate.resolved = n;
        }
    }
    if (rate.ctl) {
        const int set_ret = api_->mixer_ctl_set_value(rate.ctl, 0, static_cast<int>(cfg.rate));
        api_->mixer_ctl_set_value(rate.ctl, 1, 1);                              // mono
        api_->mixer_ctl_set_value(rate.ctl, 2, static_cast<int>(kAlsaFormatS16Le));
        api_->mixer_ctl_set_value(rate.ctl, 3, static_cast<int>(kAgmDataFormatFixedPoint));
        LOG_INFO("AgmPlayer: rate control (by " << rate.how << ", resolved '" << rate.resolved
                 << "') = " << cfg.rate << " Hz, 1 ch, fmt " << kAlsaFormatS16Le
                 << " (S16_LE), data_fmt " << kAgmDataFormatFixedPoint
                 << " (FIXED_POINT) (rc " << set_ret << ")");
    } else {
        LOG_WARN("AgmPlayer: backend rate control for '" << backend_ << "' not found on card " << kAgmCard
                 << "; the AGM PCM plugin may refuse to open. List controls with: tinymix -D " << kAgmCard);
    }

    // --- 2) backend metadata (device sub-graph KV pair registration) ---
    const std::string be_mtd_ctl_name = backend_ + " metadata";
    const CtlLookup be_mtd = find_ctl(be_mtd_ctl_name);
    if (be_mtd.ctl && api_->mixer_ctl_set_array) {
        const std::vector<uint8_t> payload = BuildBackendMetadataPayload(cfg.rate, 16);
        const int mtd_ret = api_->mixer_ctl_set_array(be_mtd.ctl, payload.data(), payload.size());
        LOG_INFO("AgmPlayer: '" << be_mtd_ctl_name << "' metadata (by " << be_mtd.how << ", "
                 << payload.size()
                 << " bytes: DEVICERX:SPEAKER, SAMPLINGRATE:" << cfg.rate << ", BITWIDTH:16) rc "
                 << mtd_ret);
    } else if (!api_->mixer_ctl_set_array) {
        LOG_WARN("AgmPlayer: vendor libtinyalsa lacks mixer_ctl_set_array; skipping backend metadata");
    } else {
        LOG_WARN("AgmPlayer: control '" << be_mtd_ctl_name << "' not found on card " << kAgmCard
                 << "; skipping backend metadata (graph open may fail with -5)");
    }

    // --- 3) stream metadata: "PCM<dev> control" = ZERO, then "PCM<dev> metadata" ---
    const std::string pcm_ctl_name = "PCM" + std::to_string(kAgmDevice) + " control";
    const CtlLookup pcm_ctl = find_ctl(pcm_ctl_name);
    if (pcm_ctl.ctl) {
        const int zero_ret = api_->mixer_ctl_set_enum_by_string(pcm_ctl.ctl, "ZERO");
        LOG_INFO("AgmPlayer: '" << pcm_ctl_name << "' (by " << pcm_ctl.how << ") -> ZERO (rc "
                 << zero_ret << ")");
    } else {
        LOG_WARN("AgmPlayer: control '" << pcm_ctl_name << "' not found on card " << kAgmCard);
    }
    const std::string pcm_mtd_ctl_name = "PCM" + std::to_string(kAgmDevice) + " metadata";
    const CtlLookup pcm_mtd = find_ctl(pcm_mtd_ctl_name);
    if (pcm_mtd.ctl && api_->mixer_ctl_set_array) {
        const char* devpp_env = getenv("AUDIOROUTER_AGM_DEVICEPP_KV");
        const bool include_devicepp = devpp_env ? (devpp_env[0] != '0') : true;
        const std::vector<uint8_t> payload = BuildStreamMetadataPayload(include_devicepp);
        const int mtd_ret = api_->mixer_ctl_set_array(pcm_mtd.ctl, payload.data(), payload.size());
        LOG_INFO("AgmPlayer: '" << pcm_mtd_ctl_name << "' metadata (by " << pcm_mtd.how << ", "
                 << payload.size()
                 << " bytes: STREAMRX:PCM_LL_PLAYBACK, INSTANCE:1" << (include_devicepp
                 ? ", DEVICEPP_RX:AUDIO_MBDRC" : "") << ", VOLUME:0) rc " << mtd_ret);
    } else if (!api_->mixer_ctl_set_array) {
        LOG_WARN("AgmPlayer: vendor libtinyalsa lacks mixer_ctl_set_array; skipping stream metadata");
    } else {
        LOG_WARN("AgmPlayer: control '" << pcm_mtd_ctl_name << "' not found on card " << kAgmCard
                 << "; skipping stream metadata (graph open may fail with -5)");
    }

    // --- 4) route PCM session onto the backend: by name, then by ID ---
    const std::string connect_ctl_name = "PCM" + std::to_string(kAgmDevice) + " connect";
    CtlLookup connect_ctl = find_ctl(connect_ctl_name);
    if (!connect_ctl.ctl && api_->mixer_get_ctl) {
        connect_ctl.ctl = api_->mixer_get_ctl(mixer_impl_, kConnectCtlId);
        connect_ctl.how = "id " + std::to_string(kConnectCtlId);
        if (connect_ctl.ctl && api_->mixer_ctl_get_name) {
            const char* n = api_->mixer_ctl_get_name(connect_ctl.ctl);
            if (n) connect_ctl.resolved = n;
        }
    }
    if (connect_ctl.ctl) {
        const int conn_ret = api_->mixer_ctl_set_enum_by_string(connect_ctl.ctl, backend_.c_str());
        LOG_INFO("AgmPlayer: '" << connect_ctl_name << "' (by " << connect_ctl.how << ", resolved '"
                 << connect_ctl.resolved << "') -> '" << backend_ << "' (rc " << conn_ret << ")");
    } else {
        LOG_WARN("AgmPlayer: control '" << connect_ctl_name << "' not found on card " << kAgmCard
                 << "; list controls with: tinymix -D " << kAgmCard);
    }

    // --- 5) optional media config on the AIF via AGM API ---
    // Disabled by default: on the A05s, control #9 already configures the AIF
    // (rate/ch/fmt/data_fmt) inside the plugin layer, and calling
    // agm_aif_set_media_config afterwards conflicts with that state (it
    // returns -EINVAL even with the correct 16-byte layout and both format
    // values). Control-9/metadata/51-only routing mirrors agmplay, which never
    // calls this API. Note the SM6225 build takes uint32_t aif_id, not the
    // backend name string this client passes.
    if (getenv("AUDIOROUTER_AGM_MEDIA_CONFIG")) {
        if (api_->agm_aif_set_media_config) {
            struct agm_media_config media_config;
            memset(&media_config, 0, sizeof(media_config));
            media_config.rate = cfg.rate;
            media_config.channels = 1;
            media_config.data_format = 0;
            int mc_ret = -22;
            for (uint32_t fmt = 0; fmt <= 1 && mc_ret != 0; ++fmt) {  // tinyalsa enum 0, alternate 1
                memset(&media_config, 0, sizeof(media_config));
                media_config.rate = cfg.rate;
                media_config.channels = 1;
                media_config.format = fmt;
                media_config.data_format = 0;
                mc_ret = api_->agm_aif_set_media_config(backend_.c_str(), &media_config);
                LOG_INFO("AgmPlayer: agm_aif_set_media_config('" << backend_ << "', " << cfg.rate
                         << " Hz, 1ch, format=" << fmt << ") rc " << mc_ret);
            }
        } else {
            LOG_WARN("AgmPlayer: AUDIOROUTER_AGM_MEDIA_CONFIG=1 but libagmclient.so is unavailable");
        }
    } else {
        LOG_INFO("AgmPlayer: skipping agm_aif_set_media_config (control 9 already configures the AIF; "
                 << "set AUDIOROUTER_AGM_MEDIA_CONFIG=1 to enable)");
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
        std::lock_guard<std::mutex> lock(io_mutex_);
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

    size_t bytes = num_frames * 2;  // mono S16
    // All state checks happen under io_mutex_ so a concurrent close() cannot
    // free the plugin handle between the check and the pcm_write call.
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!is_open_ || !api_ || !pcm_impl_) return 0;
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
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (api_ && pcm_impl_ && api_->pcm_drain) api_->pcm_drain(pcm_impl_);
#endif
}

std::string AgmPlayer::get_device_name() const {
    return "agm:" + backend_;
}

} // namespace audiorouter