#include "pulse_capture.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__) && !defined(__ANDROID__)
    #include <dirent.h>
    #include <dlfcn.h>
    #include <sys/stat.h>
    #include <sys/un.h>
    #include <unistd.h>
#endif

namespace audiorouter {

namespace {

#if defined(__linux__) && !defined(__ANDROID__)

struct PaSampleSpec {
    int format;       // PA_SAMPLE_S16LE = 3
    uint32_t rate;
    uint8_t channels;
};

struct PaBufferAttr {
    uint32_t maxlength;
    uint32_t tlength;
    uint32_t prebuf;
    uint32_t minreq;
    uint32_t fragsize;
};

struct PaServerInfo {
    const char* user_name;
    const char* host_name;
    const char* server_version;
    const char* server_name;
    PaSampleSpec sample_spec;
    const char* default_sink_name;
    const char* default_source_name;
    uint32_t cookie;
};

constexpr int kPaSampleS16LE = 3;
constexpr int kPaStreamRecord = 2;
constexpr int kPaContextNoFlags = 0;
constexpr int kPaContextReady = 4;
constexpr int kPaContextFailed = 5;
constexpr int kPaContextTerminated = 6;

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

bool path_exists(const std::string& path) {
    struct stat st{};
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

bool unix_socket_is_live(const std::string& path) {
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) return false;
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return false;
    return S_ISSOCK(st.st_mode);
}

std::string run_command(const std::string& cmd) {
    std::string out;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return out;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf) - 1, pipe)) > 0) {
        buf[n] = '\0';
        out += buf;
    }
    ::pclose(pipe);
    return out;
}

std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool command_exists(const char* name) {
    std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

struct PulseSimpleApi {
    void* handle = nullptr;
    void* (*pa_simple_new)(const char* server, const char* name, int dir, const char* dev,
                           const char* stream_name, const PaSampleSpec* ss, const void* map,
                           const PaBufferAttr* attr, int* error) = nullptr;
    int (*pa_simple_read)(void* s, void* data, size_t bytes, int* error) = nullptr;
    void (*pa_simple_free)(void* s) = nullptr;
    const char* (*pa_strerror)(int error) = nullptr;

    bool load() {
        if (handle) return true;
        const char* names[] = {"libpulse-simple.so.0", "libpulse-simple.so", nullptr};
        for (int i = 0; names[i] != nullptr; ++i) {
            handle = ::dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
            if (handle) break;
        }
        if (!handle) return false;

        pa_simple_new = reinterpret_cast<decltype(pa_simple_new)>(::dlsym(handle, "pa_simple_new"));
        pa_simple_read = reinterpret_cast<decltype(pa_simple_read)>(::dlsym(handle, "pa_simple_read"));
        pa_simple_free = reinterpret_cast<decltype(pa_simple_free)>(::dlsym(handle, "pa_simple_free"));
        pa_strerror = reinterpret_cast<decltype(pa_strerror)>(::dlsym(handle, "pa_strerror"));
        if (!pa_simple_new || !pa_simple_read || !pa_simple_free || !pa_strerror) {
            ::dlclose(handle);
            handle = nullptr;
            return false;
        }
        return true;
    }

    ~PulseSimpleApi() {
        if (handle) {
            ::dlclose(handle);
            handle = nullptr;
        }
    }
};

struct PulseCoreApi {
    void* handle = nullptr;
    void* (*pa_mainloop_new)() = nullptr;
    void (*pa_mainloop_free)(void* m) = nullptr;
    void* (*pa_mainloop_get_api)(void* m) = nullptr;
    int (*pa_mainloop_iterate)(void* m, int block, int* retval) = nullptr;
    void* (*pa_context_new)(void* mainloop, const char* name) = nullptr;
    void (*pa_context_unref)(void* c) = nullptr;
    int (*pa_context_connect)(void* c, const char* server, int flags, const void* api) = nullptr;
    void (*pa_context_disconnect)(void* c) = nullptr;
    int (*pa_context_get_state)(void* c) = nullptr;
    void* (*pa_context_get_server_info)(void* c, void (*cb)(void* c, const PaServerInfo* i, void* userdata), void* userdata) = nullptr;
    void (*pa_operation_unref)(void* o) = nullptr;

    bool load() {
        if (handle) return true;
        const char* names[] = {"libpulse.so.0", "libpulse.so", nullptr};
        for (int i = 0; names[i] != nullptr; ++i) {
            handle = ::dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
            if (handle) break;
        }
        if (!handle) return false;

        pa_mainloop_new = reinterpret_cast<decltype(pa_mainloop_new)>(::dlsym(handle, "pa_mainloop_new"));
        pa_mainloop_free = reinterpret_cast<decltype(pa_mainloop_free)>(::dlsym(handle, "pa_mainloop_free"));
        pa_mainloop_get_api = reinterpret_cast<decltype(pa_mainloop_get_api)>(::dlsym(handle, "pa_mainloop_get_api"));
        pa_mainloop_iterate = reinterpret_cast<decltype(pa_mainloop_iterate)>(::dlsym(handle, "pa_mainloop_iterate"));
        pa_context_new = reinterpret_cast<decltype(pa_context_new)>(::dlsym(handle, "pa_context_new"));
        pa_context_unref = reinterpret_cast<decltype(pa_context_unref)>(::dlsym(handle, "pa_context_unref"));
        pa_context_connect = reinterpret_cast<decltype(pa_context_connect)>(::dlsym(handle, "pa_context_connect"));
        pa_context_disconnect = reinterpret_cast<decltype(pa_context_disconnect)>(::dlsym(handle, "pa_context_disconnect"));
        pa_context_get_state = reinterpret_cast<decltype(pa_context_get_state)>(::dlsym(handle, "pa_context_get_state"));
        pa_context_get_server_info = reinterpret_cast<decltype(pa_context_get_server_info)>(::dlsym(handle, "pa_context_get_server_info"));
        pa_operation_unref = reinterpret_cast<decltype(pa_operation_unref)>(::dlsym(handle, "pa_operation_unref"));

        if (!pa_mainloop_new || !pa_mainloop_free || !pa_mainloop_get_api || !pa_mainloop_iterate ||
            !pa_context_new || !pa_context_unref || !pa_context_connect || !pa_context_disconnect ||
            !pa_context_get_state || !pa_context_get_server_info || !pa_operation_unref) {
            ::dlclose(handle);
            handle = nullptr;
            return false;
        }
        return true;
    }

    ~PulseCoreApi() {
        if (handle) {
            ::dlclose(handle);
            handle = nullptr;
        }
    }
};

struct ServerInfoQuery {
    bool done = false;
    std::string default_sink;
};

void on_server_info(void* /*c*/, const PaServerInfo* info, void* userdata) {
    auto* q = static_cast<ServerInfoQuery*>(userdata);
    if (info && info->default_sink_name) {
        q->default_sink = info->default_sink_name;
    }
    q->done = true;
}

#endif // __linux__ && !__ANDROID__

} // namespace

struct PulseCapture::Impl {
#if defined(__linux__) && !defined(__ANDROID__)
    PulseSimpleApi simple_api;
    void* stream = nullptr;
    FILE* parec_pipe = nullptr;
#endif
};

PulseCapture::PulseCapture(std::string source_spec)
    : impl_(std::make_unique<Impl>()),
      source_spec_(parse_source_spec(source_spec)),
      is_running_(false),
      device_name_("PulseAudio Monitor") {}

PulseCapture::~PulseCapture() {
    stop();
}

std::string PulseCapture::parse_source_spec(const std::string& device_spec) {
    if (device_spec.empty() || device_spec == "auto" ||
        device_spec == "pulse" || device_spec == "pulseaudio" || device_spec == "pa") {
        return "";
    }
    for (const char* prefix : {"pulseaudio:", "pulse:", "pa:"}) {
        const size_t len = std::strlen(prefix);
        if (device_spec.compare(0, len, prefix) == 0) {
            std::string rest = device_spec.substr(len);
            if (rest == "default" || rest == "@DEFAULT_MONITOR@") {
                return "";
            }
            return rest;
        }
    }
    return device_spec;
}

bool PulseCapture::is_supported() {
#if defined(__linux__) && !defined(__ANDROID__)
    PulseSimpleApi api;
    return api.load() || command_exists("parec");
#else
    return false;
#endif
}

std::string PulseCapture::resolve_server_address() {
#if defined(__linux__) && !defined(__ANDROID__)
    const std::string env_server = env_or_empty("PULSE_SERVER");
    if (!env_server.empty()) {
        std::string unix_path;
        if (env_server.rfind("unix:", 0) == 0) {
            unix_path = env_server.substr(5);
        } else if (env_server[0] == '/') {
            unix_path = env_server;
        }
        if (unix_path.empty() || unix_socket_is_live(unix_path)) {
            return env_server;
        }
    }

    std::vector<std::string> candidates;
    const std::string xdg = env_or_empty("XDG_RUNTIME_DIR");
    if (!xdg.empty()) candidates.push_back(xdg + "/pulse/native");
    candidates.push_back("/run/user/" + std::to_string(static_cast<unsigned>(::getuid())) + "/pulse/native");

    const std::string home = env_or_empty("HOME");
    if (!home.empty()) {
        candidates.push_back(home + "/.config/pulse/native");
        candidates.push_back(home + "/.pulse/native");
    }

    for (const auto& c : candidates) {
        if (path_exists(c) && unix_socket_is_live(c)) {
            return "unix:" + c;
        }
    }
    return "";
#else
    return "";
#endif
}

bool PulseCapture::server_available() {
    return !resolve_server_address().empty();
}

std::string PulseCapture::resolve_default_monitor_source(const std::string& server) {
#if defined(__linux__) && !defined(__ANDROID__)
    PulseCoreApi core;
    if (core.load()) {
        void* ml = core.pa_mainloop_new();
        if (ml) {
            void* ml_api = core.pa_mainloop_get_api(ml);
            void* ctx = core.pa_context_new(ml_api, "AudioRouterServerProbe");
            if (ctx) {
                const char* srv = server.empty() ? nullptr : server.c_str();
                if (core.pa_context_connect(ctx, srv, kPaContextNoFlags, nullptr) >= 0) {
                    const uint64_t deadline_ms = get_time_ms() + 1500;
                    bool op_started = false;
                    void* op = nullptr;
                    ServerInfoQuery query;

                    while (get_time_ms() < deadline_ms) {
                        int retval = 0;
                        core.pa_mainloop_iterate(ml, 0, &retval);
                        int st = core.pa_context_get_state(ctx);
                        if (st == kPaContextFailed || st == kPaContextTerminated) {
                            break;
                        }
                        if (st == kPaContextReady && !op_started) {
                            op_started = true;
                            op = core.pa_context_get_server_info(ctx, on_server_info, &query);
                        }
                        if (query.done) {
                            break;
                        }
                        sleep_ms(5);
                    }
                    if (op) core.pa_operation_unref(op);
                    core.pa_context_disconnect(ctx);
                    core.pa_context_unref(ctx);
                    core.pa_mainloop_free(ml);

                    if (!query.default_sink.empty()) {
                        return query.default_sink + ".monitor";
                    }
                } else {
                    core.pa_context_unref(ctx);
                    core.pa_mainloop_free(ml);
                }
            } else {
                core.pa_mainloop_free(ml);
            }
        }
    }

    // Fallback 1: pactl get-default-sink (PulseAudio >= 15 / PipeWire)
    const std::string sink = trim(run_command("pactl get-default-sink 2>/dev/null"));
    if (!sink.empty()) {
        return sink + ".monitor";
    }

    // Fallback 2: pactl info (works on older PulseAudio < 15 as well)
    const std::string info = run_command("pactl info 2>/dev/null");
    const std::string needle = "Default Sink:";
    const size_t pos = info.find(needle);
    if (pos != std::string::npos) {
        size_t start = pos + needle.size();
        size_t eol = info.find('\n', start);
        std::string s = trim(info.substr(start, eol == std::string::npos ? std::string::npos : eol - start));
        if (!s.empty()) {
            return s + ".monitor";
        }
    }

    // Fallback 3: first *.monitor source from pactl list short sources
    for (const auto& src : list_sources()) {
        if (src.size() > 8 && src.compare(src.size() - 8, 8, ".monitor") == 0) {
            return src;
        }
    }
    return "";
#else
    (void)server;
    return "";
#endif
}

std::vector<std::string> PulseCapture::list_sources() {
    std::vector<std::string> sources;
#if defined(__linux__) && !defined(__ANDROID__)
    const std::string out = run_command("pactl list short sources 2>/dev/null");
    size_t pos = 0;
    while (pos < out.size()) {
        size_t eol = out.find('\n', pos);
        if (eol == std::string::npos) eol = out.size();
        const std::string line = out.substr(pos, eol - pos);
        pos = eol + 1;
        const size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        const size_t t2 = line.find('\t', t1 + 1);
        const std::string name = line.substr(t1 + 1, (t2 == std::string::npos ? line.size() : t2) - t1 - 1);
        if (!name.empty()) sources.push_back(name);
    }
#endif
    return sources;
}

bool PulseCapture::start(const AudioConfig& desired_config, AudioConfig& actual_config) {
    if (is_running_) {
        actual_config = actual_config_;
        return true;
    }

#if defined(__linux__) && !defined(__ANDROID__)
    const std::string server = resolve_server_address();
    if (server.empty()) {
        LOG_ERROR("PulseCapture: No reachable PulseAudio/PipeWire-Pulse daemon socket found.");
        return false;
    }

    std::string source = source_spec_;
    if (source.empty()) {
        source = resolve_default_monitor_source(server);
        if (source.empty()) {
            LOG_ERROR("PulseCapture: Could not determine default sink monitor source. "
                      "Specify one explicitly with '-d pulse:<source_name>'.");
            return false;
        }
    }

    actual_config_.sample_rate = desired_config.sample_rate > 0 ? desired_config.sample_rate : 48000;
    actual_config_.channels = desired_config.channels > 0 ? desired_config.channels : 2;
    actual_config_.format = AudioSampleFormat::PCM_S16LE;
    actual_config_.frames_per_packet = desired_config.frames_per_packet > 0 ? desired_config.frames_per_packet : 240;

    if (impl_->simple_api.load()) {
        PaSampleSpec ss{};
        ss.format = kPaSampleS16LE;
        ss.rate = actual_config_.sample_rate;
        ss.channels = static_cast<uint8_t>(actual_config_.channels);

        const uint32_t frag_bytes = static_cast<uint32_t>(
            actual_config_.frames_per_packet * actual_config_.bytes_per_frame());
        PaBufferAttr attr{};
        attr.maxlength = frag_bytes * 4;
        attr.tlength = static_cast<uint32_t>(-1);
        attr.prebuf = static_cast<uint32_t>(-1);
        attr.minreq = static_cast<uint32_t>(-1);
        attr.fragsize = frag_bytes;

        int error = 0;
        void* stream = impl_->simple_api.pa_simple_new(
            server.c_str(),
            "AudioRouter",
            kPaStreamRecord,
            source.c_str(),
            "System audio loopback",
            &ss,
            nullptr,
            &attr,
            &error);

        if (!stream) {
            LOG_ERROR("PulseCapture: pa_simple_new failed for source '" << source
                      << "' on server '" << server << "': "
                      << impl_->simple_api.pa_strerror(error));
            return false;
        }
        impl_->stream = stream;
    } else if (command_exists("parec")) {
        std::string cmd = "parec --raw --format=s16le --rate=" +
                          std::to_string(actual_config_.sample_rate) +
                          " --channels=" + std::to_string(actual_config_.channels) +
                          " --latency-msec=10 -d '" + source + "' 2>/dev/null";
        impl_->parec_pipe = ::popen(cmd.c_str(), "r");
        if (!impl_->parec_pipe) {
            LOG_ERROR("PulseCapture: Failed to launch parec for source '" << source << "'");
            return false;
        }
        LOG_INFO("PulseCapture: Using parec CLI fallback (libpulse-simple.so.0 not installed)");
    } else {
        LOG_ERROR("PulseCapture: Neither libpulse-simple.so.0 nor parec is available.");
        return false;
    }

    device_name_ = source;
    actual_config = actual_config_;

    is_running_ = true;
    capture_thread_ = std::thread(&PulseCapture::capture_thread_func, this);
    LOG_INFO("PulseCapture: Loopback capture active on '" << device_name_ << "' ("
             << actual_config_.to_string() << ")");
    return true;
#else
    (void)desired_config;
    (void)actual_config;
    return false;
#endif
}

void PulseCapture::stop() {
    if (!is_running_) return;
    is_running_ = false;
#if defined(__linux__) && !defined(__ANDROID__)
    if (impl_->parec_pipe) {
        ::pclose(impl_->parec_pipe);
        impl_->parec_pipe = nullptr;
    }
#endif
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
#if defined(__linux__) && !defined(__ANDROID__)
    if (impl_->stream && impl_->simple_api.pa_simple_free) {
        impl_->simple_api.pa_simple_free(impl_->stream);
        impl_->stream = nullptr;
    }
#endif
    LOG_INFO("PulseCapture: Loopback capture stopped");
}

bool PulseCapture::is_capturing() const {
    return is_running_;
}

void PulseCapture::set_audio_callback(AudioCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_ = std::move(cb);
}

std::string PulseCapture::get_device_name() const {
    return device_name_;
}

void PulseCapture::capture_thread_func() {
#if defined(__linux__) && !defined(__ANDROID__)
    const size_t frames_per_block = actual_config_.frames_per_packet;
    const size_t bytes_per_block = frames_per_block * actual_config_.bytes_per_frame();
    std::vector<uint8_t> buf(bytes_per_block);

    while (is_running_) {
        if (impl_->stream) {
            int error = 0;
            if (impl_->simple_api.pa_simple_read(impl_->stream, buf.data(), buf.size(), &error) < 0) {
                if (!is_running_) break;
                LOG_WARN("PulseCapture: pa_simple_read failed: "
                         << impl_->simple_api.pa_strerror(error));
                sleep_ms(10);
                continue;
            }
        } else if (impl_->parec_pipe) {
            size_t total = 0;
            while (total < buf.size() && is_running_) {
                size_t n = std::fread(buf.data() + total, 1, buf.size() - total, impl_->parec_pipe);
                if (n == 0) break;
                total += n;
            }
            if (total < buf.size()) {
                if (!is_running_) break;
                sleep_ms(10);
                continue;
            }
        } else {
            break;
        }

        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (callback_) {
            callback_(buf.data(), frames_per_block, actual_config_);
        }
    }
#endif
}

} // namespace audiorouter
