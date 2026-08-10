#include "agm_player.hpp"
#include "android_helpers.hpp"
#include "../common/logger.hpp"

#include <dlfcn.h>
#include <dirent.h>
#include <cstring>
#include <atomic>
#include <vector>

// The AGM client library is a Qualcomm vendor library not present in the NDK
// or Termux; the API contract below matches agm_api.h shipped in
// hardware/qcom/audio/agm (libagmclient symbols). The struct/flag values are
// ABI constants of that header.
namespace {

enum agm_session_mode {
    AGM_SESSION_MODE_RX = 1,
    AGM_SESSION_MODE_TX = 2,
};

enum agm_media_format {
    AGM_MEDIA_FORMAT_PCM_16_BIT = 0,
};

struct agm_media_config {
    uint32_t rate;
    uint32_t channels;
    uint32_t format;
};

typedef int (*agm_aif_set_media_config_fn)(const char* aif_name, struct agm_media_config* media_config);
typedef int (*agm_session_open_fn)(uint32_t session_id, uint32_t mode, uint64_t* session_handle);
typedef int (*agm_session_prepare_fn)(uint64_t session_handle);
typedef int (*agm_session_start_fn)(uint64_t session_handle);
typedef int (*agm_session_write_fn)(uint64_t session_handle, void* buf, size_t* byte_count);
typedef int (*agm_session_stop_fn)(uint64_t session_handle);
typedef int (*agm_session_close_fn)(uint64_t session_handle);

// Session ids start at 100 (the id used by the vendor's agmplay tool) and
// increment per attempt so a hung/abandoned attempt can't collide with a fresh
// one in the AGM service.
std::atomic<uint32_t> g_next_session_id{100};

// Qualcomm ships the AGM client in the vendor partition; the bare name is only
// a last resort for setups that add it to the linker path.
const char* kAgmLibCandidates[] = {
    "/vendor/lib64/libagmclient.so",
    "/vendor/lib/libagmclient.so",
    "/system/lib64/libagmclient.so",
    "/system/lib/libagmclient.so",
    "/system_ext/lib64/libagmclient.so",
    "/odm/lib64/libagmclient.so",
    "libagmclient.so",
};

// Samsung / budget Qualcomm builds sometimes rename or relocate the AGM
// client; list every AGM-shaped library found on the device so the failure
// becomes actionable instead of a dead end.
std::vector<std::string> scan_for_agm_libraries() {
    std::vector<std::string> found;
    const char* dirs[] = {
        "/vendor/lib64", "/vendor/lib",
        "/system/lib64", "/system/lib",
        "/system_ext/lib64", "/odm/lib64",
    };
    for (const char* dir : dirs) {
        DIR* d = opendir(dir);
        if (!d) continue;
        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
            const std::string name = entry->d_name;
            if (name.find("agm") != std::string::npos || name.find("AGM") != std::string::npos) {
                found.push_back(std::string(dir) + "/" + name);
            }
        }
        closedir(d);
    }
    return found;
}

// When agm_session_open fails, dump the AGM service state so the failure is
// actionable: binder service registered? daemon running?
void log_agm_service_status() {
    FILE* pipe = popen("service list 2>/dev/null | grep -i agm; ps -A 2>/dev/null | grep -i agm", "r");
    if (!pipe) return;
    char line[256];
    while (fgets(line, sizeof(line), pipe)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) LOG_ERROR("AgmPlayer:   " << s);
    }
    pclose(pipe);
}

} // namespace

namespace audiorouter {

struct AgmPlayer::AgmApi {
    void* handle = nullptr;
    agm_aif_set_media_config_fn aif_set_media_config = nullptr;
    agm_session_open_fn session_open = nullptr;
    agm_session_prepare_fn session_prepare = nullptr;
    agm_session_start_fn session_start = nullptr;
    agm_session_write_fn session_write = nullptr;
    agm_session_stop_fn session_stop = nullptr;
    agm_session_close_fn session_close = nullptr;
};

AgmPlayer::AgmPlayer() : api_(nullptr), session_handle_(0), is_open_(false) {}

AgmPlayer::~AgmPlayer() {
    close();
    unload_agm_library();
}

bool AgmPlayer::load_agm_library() {
    if (api_ && api_->handle) return true;
    if (api_) {
        delete api_;
        api_ = nullptr;
    }

    void* handle = nullptr;
    const char* loaded_path = nullptr;
    for (const char* path : kAgmLibCandidates) {
        handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (handle) {
            loaded_path = path;
            break;
        }
    }
    if (!handle) {
        LOG_ERROR("AgmPlayer: could not dlopen libagmclient.so: " << dlerror());
        LOG_ERROR("AgmPlayer: run this binary via 'su' (root). Plain Termux processes cannot load vendor libraries.");

        const std::vector<std::string> agm_libs = scan_for_agm_libraries();
        bool found_exact = false;
        for (const auto& lib : agm_libs) {
            if (lib.size() >= 16 && lib.compare(lib.size() - 16, 16, "libagmclient.so") == 0) {
                found_exact = true;
                break;
            }
        }

        if (found_exact) {
            LOG_ERROR("AgmPlayer: libagmclient.so EXISTS on this device but dlopen failed - the linker "
                      << "namespace likely blocks vendor paths and their dependencies. Run with:");
            LOG_ERROR("AgmPlayer:   su -c \"LD_LIBRARY_PATH=/vendor/lib64 ./audiorouter_client -s <PC_IP> -d agm\"");
        } else if (agm_libs.empty()) {
            LOG_ERROR("AgmPlayer: no AGM libraries exist under /vendor, /system, /system_ext or /odm. This build "
                      << "uses the classic ALSA HAL; the client will fall back to direct /dev/snd PCM nodes "
                      << "(run: stop audioserver, then retry).");
        } else {
            LOG_ERROR("AgmPlayer: AGM-related libraries found, but none named libagmclient.so:");
            for (const auto& lib : agm_libs) {
                LOG_ERROR("    " << lib);
            }
        }
        return false;
    }

    auto* api = new AgmApi();
    api->handle = handle;
    api->aif_set_media_config =
        reinterpret_cast<agm_aif_set_media_config_fn>(dlsym(handle, "agm_aif_set_media_config"));
    api->session_open = reinterpret_cast<agm_session_open_fn>(dlsym(handle, "agm_session_open"));
    api->session_prepare = reinterpret_cast<agm_session_prepare_fn>(dlsym(handle, "agm_session_prepare"));
    api->session_start = reinterpret_cast<agm_session_start_fn>(dlsym(handle, "agm_session_start"));
    api->session_write = reinterpret_cast<agm_session_write_fn>(dlsym(handle, "agm_session_write"));
    api->session_stop = reinterpret_cast<agm_session_stop_fn>(dlsym(handle, "agm_session_stop"));
    api->session_close = reinterpret_cast<agm_session_close_fn>(dlsym(handle, "agm_session_close"));

    if (!api->aif_set_media_config || !api->session_open || !api->session_prepare || !api->session_start ||
        !api->session_write || !api->session_stop || !api->session_close) {
        LOG_ERROR("AgmPlayer: libagmclient.so is missing required AGM symbols: " << dlerror());
        dlclose(handle);
        delete api;
        return false;
    }

    api_ = api;
    LOG_INFO("AgmPlayer: loaded libagmclient.so from '" << loaded_path << "'");
    return true;
}

void AgmPlayer::unload_agm_library() {
    if (api_) {
        if (api_->handle) dlclose(api_->handle);
        delete api_;
        api_ = nullptr;
    }
}

bool AgmPlayer::open_session(const AudioConfig& config, const std::string& backend) {
    uint32_t session_id = g_next_session_id.fetch_add(1);
    uint64_t handle = 0;
    int rc = api_->session_open(session_id, AGM_SESSION_MODE_RX, &handle);
    if (rc != 0 || handle == 0) {
        LOG_ERROR("AgmPlayer: agm_session_open(" << session_id << ", RX) returned " << rc
                  << " (handle " << handle << ").");
        LOG_ERROR("AgmPlayer: the vendor AGM service may be down or not exposed to this process. "
                  << "Check whether agm_service is running and registered:");
        log_agm_service_status();
        return false;
    }

    struct agm_media_config media_config;
    std::memset(&media_config, 0, sizeof(media_config));
    media_config.rate = config.sample_rate;
    // The CODEC_DMA-LPAIF_RXTX-RX-1 graph is mono; stereo streams are downmixed
    // in write_frames().
    media_config.channels = 1;
    media_config.format = AGM_MEDIA_FORMAT_PCM_16_BIT;

    int ret = api_->aif_set_media_config(backend.c_str(), &media_config);
    if (ret != 0) {
        LOG_ERROR("AgmPlayer: agm_aif_set_media_config('" << backend << "') failed with " << ret
                  << ". Set the backend name or stop audioserver and retry.");
        api_->session_close(handle);
        return false;
    }

    ret = api_->session_prepare(handle);
    if (ret != 0) {
        LOG_ERROR("AgmPlayer: agm_session_prepare failed with " << ret
                  << ". The DSP graph could not be created on backend '" << backend << "'.");
        api_->session_close(handle);
        return false;
    }

    ret = api_->session_start(handle);
    if (ret != 0) {
        LOG_ERROR("AgmPlayer: agm_session_start failed with " << ret);
        api_->session_stop(handle);
        api_->session_close(handle);
        return false;
    }

    session_handle_ = handle;
    backend_ = backend;
    LOG_INFO("AgmPlayer: session " << session_id << " started on backend '" << backend << "' ("
             << config.to_string() << ", mono output)");
    return true;
}

bool AgmPlayer::open(const AudioConfig& config, const std::string& device_name) {
#if defined(__linux__) || defined(__ANDROID__)
    if (is_open_) close();

    config_ = config;
    std::string backend = "CODEC_DMA-LPAIF_RXTX-RX-1";
    const std::string name = device_name.empty() ? "agm" : device_name;
    if (name.rfind("agm:", 0) == 0) {
        std::string rest = name.substr(4);
        if (!rest.empty()) backend = rest;
    }

    if (!load_agm_library()) return false;

    // Route the codec to the speaker before starting the session. Best effort:
    // if audioserver re-routes the mixer this is redone on the next open.
    AndroidHelpers::apply_speaker_routing();

    if (!open_session(config, backend)) return false;

    is_open_ = true;
    LOG_INFO("AgmPlayer: open complete on backend '" << backend_ << "'");
    return true;
#else
    (void)config;
    (void)device_name;
    LOG_INFO("AgmPlayer: AGM playback not available on this platform");
    return false;
#endif
}

void AgmPlayer::close() {
    is_open_ = false;
#if defined(__linux__) || defined(__ANDROID__)
    if (api_ && session_handle_ != 0) {
        api_->session_stop(session_handle_);
        api_->session_close(session_handle_);
        session_handle_ = 0;
        LOG_INFO("AgmPlayer: session closed");
    }
#endif
}

bool AgmPlayer::is_open() const {
    return is_open_;
}

size_t AgmPlayer::write_frames(const void* pcm_data, size_t num_frames) {
    if (!is_open_ || !pcm_data || num_frames == 0) return 0;
#if defined(__linux__) || defined(__ANDROID__)
    if (!api_ || session_handle_ == 0) return 0;

    const uint64_t handle = session_handle_;
    const size_t in_channels = (config_.channels > 0) ? config_.channels : 2;
    const size_t out_channels = 1;  // mono backend
    const size_t out_frame_bytes = out_channels * 2;

    const int16_t* src = reinterpret_cast<const int16_t*>(pcm_data);
    if (in_channels == 2) {
        downmix_buffer_.resize(num_frames);
        for (size_t i = 0; i < num_frames; ++i) {
            const int32_t l = src[i * 2];
            const int32_t r = src[i * 2 + 1];
            downmix_buffer_[i] = static_cast<int16_t>((l + r) / 2);
        }
        src = downmix_buffer_.data();
    }

    size_t bytes_left = num_frames * out_frame_bytes;
    size_t chunk_bytes = 2048 * out_frame_bytes;  // ~43 ms @ 48 kHz

    while (bytes_left > 0 && is_open_ && session_handle_ != 0) {
        size_t chunk = (bytes_left < chunk_bytes) ? bytes_left : chunk_bytes;
        size_t written = chunk;
        int ret = api_->session_write(handle, const_cast<int16_t*>(src), &written);
        if (ret != 0) {
            LOG_DEBUG("AgmPlayer: agm_session_write returned " << ret << " (" << strerror(errno) << ")");
            return (num_frames * out_frame_bytes - bytes_left) / out_frame_bytes;
        }
        if (written == 0 || written > chunk) break;  // no progress - avoid spinning
        src += written / out_frame_bytes * out_channels;
        bytes_left -= written;
    }

    return num_frames;
#else
    (void)pcm_data;
    (void)num_frames;
    return 0;
#endif
}

size_t AgmPlayer::get_buffer_delay_frames() const {
    // The AGM client does not expose the DSP buffer level; report none.
    return 0;
}

void AgmPlayer::flush() {
    // Nothing to drop: session_write is paced by the DSP calendar.
}

std::string AgmPlayer::get_device_name() const {
    return "agm:" + backend_;
}

} // namespace audiorouter