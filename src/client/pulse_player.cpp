#include "pulse_player.hpp"

#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(PULSEAUDIO_ENABLED)
    #include <pulse/error.h>
    #include <pulse/sample.h>
    #include <pulse/simple.h>
#endif

#if !defined(_WIN32)
    #include <cerrno>
    #include <dirent.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <sys/stat.h>
    #include <sys/un.h>
    #include <unistd.h>
#endif

namespace audiorouter {

namespace pulse {

namespace {

// Trims ASCII whitespace from both ends.
std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

} // namespace

bool is_pulse_device_name(const std::string& device_name) {
    return device_name == "pulse" || device_name == "pulseaudio" || device_name == "pa" ||
           device_name.rfind("pulse:", 0) == 0 || device_name.rfind("pulse@", 0) == 0 ||
           device_name.rfind("pulseaudio:", 0) == 0 || device_name.rfind("pulseaudio@", 0) == 0 ||
           device_name.rfind("pa:", 0) == 0 || device_name.rfind("pa@", 0) == 0;
}

DeviceSpec parse_device_spec(const std::string& device_name) {
    DeviceSpec spec;
    if (!is_pulse_device_name(device_name)) {
        return spec;
    }

    // Strip the backend prefix ("pulse", "pulseaudio" or "pa"); what is left
    // is either empty, ":<sink>", "@<ms>" or ":<sink>@<ms>".
    std::string rest;
    for (const char* prefix : {"pulseaudio", "pulse", "pa"}) {
        const size_t len = std::strlen(prefix);
        if (device_name.compare(0, len, prefix) == 0) {
            rest = device_name.substr(len);
            break;
        }
    }

    // The latency suffix is the LAST '@' — PulseAudio sink names never contain
    // one, but "@DEFAULT_SINK@" does, so searching from the right and
    // requiring digits keeps that spelling working.
    const size_t at = rest.rfind('@');
    if (at != std::string::npos) {
        const std::string tail = trim(rest.substr(at + 1));
        const bool all_digits = !tail.empty() &&
            std::all_of(tail.begin(), tail.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; });
        if (all_digits) {
            unsigned long ms = 0;
            try {
                ms = std::stoul(tail);
            } catch (const std::exception&) {
                ms = 0;
            }
            if (ms > kMaxLatencyMs) ms = kMaxLatencyMs;
            if (ms != 0 && ms < kMinLatencyMs) ms = kMinLatencyMs;
            spec.latency_ms = static_cast<uint32_t>(ms);
            rest = rest.substr(0, at);
        }
    }

    if (!rest.empty() && rest.front() == ':') {
        spec.sink = trim(rest.substr(1));
    }

    // "default" / "@DEFAULT_SINK@" mean "whatever the daemon prefers", which
    // libpulse expresses as a null device.
    if (spec.sink == "default" || spec.sink == "@DEFAULT_SINK@") {
        spec.sink.clear();
    }
    return spec;
}

BufferAttr compute_buffer_attr(uint32_t sample_rate, size_t bytes_per_frame,
                               uint32_t frames_per_packet, uint32_t latency_ms) {
    BufferAttr attr;
    if (sample_rate == 0 || bytes_per_frame == 0) {
        return attr;
    }
    if (frames_per_packet == 0) {
        frames_per_packet = 240;  // 5 ms @ 48 kHz, the server default
    }

    const uint64_t packet_bytes = static_cast<uint64_t>(frames_per_packet) * bytes_per_frame;
    const uint64_t byte_rate = static_cast<uint64_t>(sample_rate) * bytes_per_frame;

    if (latency_ms == 0) {
        // Four packets of headroom: enough for the playback thread to be
        // descheduled a few periods without the sink running dry, while the
        // jitter buffer keeps owning network jitter.
        const uint64_t derived = (packet_bytes * 4 * 1000 + byte_rate - 1) / byte_rate;
        latency_ms = static_cast<uint32_t>(derived);
    }
    latency_ms = std::clamp(latency_ms, kMinLatencyMs, kMaxLatencyMs);

    uint64_t tlength = (byte_rate * latency_ms) / 1000;
    // Never ask for less than one packet, or a single write would exceed the
    // whole buffer and stall.
    tlength = std::max<uint64_t>(tlength, packet_bytes);
    // Keep the target frame-aligned; PulseAudio rounds internally, but an
    // aligned request keeps the reported latency predictable.
    tlength -= tlength % bytes_per_frame;

    attr.tlength = static_cast<uint32_t>(tlength);
    attr.minreq = static_cast<uint32_t>(packet_bytes);
    // Start playing as soon as one packet is queued: prebuf is the only place
    // where PulseAudio would add a fixed startup delay of its own.
    attr.prebuf = static_cast<uint32_t>(packet_bytes);
    // Cap the hard buffer at twice the target so a burst (the jitter buffer's
    // prefill) cannot silently build a large permanent backlog.
    attr.maxlength = static_cast<uint32_t>(std::min<uint64_t>(tlength * 2, 0xFFFFFFFFull));
    return attr;
}

} // namespace pulse

namespace {

#if defined(PULSEAUDIO_ENABLED)
// Maps the wire format to a PulseAudio sample format. Every format the
// protocol can negotiate has an exact PulseAudio equivalent.
pa_sample_format_t to_pa_format(AudioSampleFormat fmt) {
    switch (fmt) {
        case AudioSampleFormat::PCM_S16LE:     return PA_SAMPLE_S16LE;
        case AudioSampleFormat::PCM_FLOAT32LE: return PA_SAMPLE_FLOAT32LE;
        case AudioSampleFormat::PCM_S24LE:     return PA_SAMPLE_S24LE;
        case AudioSampleFormat::PCM_S32LE:     return PA_SAMPLE_S32LE;
        default:                               return PA_SAMPLE_INVALID;
    }
}
#endif

// Reads a whole command's stdout. Empty on failure.
std::string run_command(const std::string& cmd) {
    std::string out;
#if defined(_WIN32)
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return out;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf) - 1, pipe)) > 0) {
        buf[n] = '\0';
        out += buf;
    }
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return out;
}

#if !defined(_WIN32)
bool path_exists(const std::string& path) {
    struct stat st{};
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

// Is a PulseAudio daemon actually LISTENING on this AF_UNIX path?
//
// The mere existence of the socket inode proves nothing: a daemon that died
// (or was killed by Android) leaves the file behind, and connecting to it
// gets ECONNREFUSED. Worse, a half-alive daemon can accept the connection and
// then never answer, which is what makes pa_simple_new() block. A real
// non-blocking connect() answers both questions in microseconds.
bool unix_socket_is_live(const std::string& path) {
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) return false;

    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return false;
    if (!S_ISSOCK(st.st_mode)) return false;   // pid files etc. are not sockets

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    bool live = false;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        live = true;                    // connected straight away
    } else if (errno == EINPROGRESS || errno == EAGAIN) {
        // Non-blocking connect in flight: give it a brief, bounded moment.
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        if (::poll(&pfd, 1, 200) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            live = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
        }
    }
    ::close(fd);
    return live;
}
#endif

const char* env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

// Reconnect attempts are rate-limited to this interval so a dead daemon
// cannot turn the playback thread into a busy loop.
constexpr uint64_t kReconnectIntervalMs = 2000;
// Write errors are logged at most this often.
constexpr uint64_t kErrorLogIntervalMs = 5000;
// Consecutive failed writes before the connection is torn down and rebuilt.
constexpr int kWriteFailuresBeforeReconnect = 3;

} // namespace

PulsePlayer::PulsePlayer() = default;

PulsePlayer::~PulsePlayer() {
    close();
}

bool PulsePlayer::is_supported() {
#if defined(PULSEAUDIO_ENABLED)
    return true;
#else
    return false;
#endif
}

bool PulsePlayer::server_available() {
    return !resolve_server_address().empty();
}

std::string PulsePlayer::resolve_server_address() {
#if defined(_WIN32)
    return env_or_empty("PULSE_SERVER");
#else
    // An explicit server address normally wins: it may be a TCP endpoint with
    // no local socket at all (a common Termux setup points at 127.0.0.1:4713),
    // which we cannot validate here, so it is taken at face value.
    //
    // The one case we CAN check is a unix: path: if the environment names a
    // socket that no daemon is listening on (a stale export left over from an
    // earlier session), honouring it blindly would mask the daemon that is
    // actually running. Fall through to discovery in that case.
    {
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
            LOG_WARN("PulsePlayer: PULSE_SERVER points at '" << env_server
                     << "' but no daemon is listening there; ignoring it and looking for "
                     "the running daemon instead.");
        }
    }

    std::vector<std::string> candidates;
    const std::string xdg = env_or_empty("XDG_RUNTIME_DIR");
    if (!xdg.empty()) candidates.push_back(xdg + "/pulse/native");

    candidates.push_back("/run/user/" + std::to_string(static_cast<unsigned>(::getuid())) + "/pulse/native");

    // Termux installs the daemon under $PREFIX and runs it without a
    // /run/user tree.
    const std::string prefix = env_or_empty("PREFIX");
    if (!prefix.empty()) {
        candidates.push_back(prefix + "/var/run/pulse/native");
        candidates.push_back(prefix + "/var/run/pulse/pid");
    }
    const std::string home = env_or_empty("HOME");
    if (!home.empty()) {
        candidates.push_back(home + "/.pulse/native");
        candidates.push_back(home + "/.config/pulse/native");
        // Termux's PulseAudio has no /run/user tree, so it puts its runtime
        // directory under the state directory, keyed by machine ID:
        //   $HOME/.config/pulse/<machine-id>-runtime/native
        // The machine ID is not knowable up front, so read it when available
        // and also glob for any *-runtime directory (a stale ID can linger
        // after a reinstall; the liveness check below sorts out which is real).
        for (const std::string& base : {home + "/.config/pulse", home + "/.pulse"}) {
            std::string machine_id;
            for (const std::string& id_file : {base + "/machine-id",
                                               std::string("/etc/machine-id")}) {
                if (FILE* f = std::fopen(id_file.c_str(), "r")) {
                    char buf[64] = {0};
                    if (std::fgets(buf, sizeof(buf), f) != nullptr) {
                        machine_id = buf;
                        while (!machine_id.empty() &&
                               (machine_id.back() == '\n' || machine_id.back() == '\r' ||
                                machine_id.back() == ' ')) {
                            machine_id.pop_back();
                        }
                    }
                    std::fclose(f);
                    if (!machine_id.empty()) break;
                }
            }
            if (!machine_id.empty()) {
                candidates.push_back(base + "/" + machine_id + "-runtime/native");
            }
            if (DIR* d = ::opendir(base.c_str())) {
                std::vector<std::string> runtime_dirs;
                while (const dirent* e = ::readdir(d)) {
                    const std::string name = e->d_name;
                    if (name.size() > 8 &&
                        name.compare(name.size() - 8, 8, "-runtime") == 0) {
                        runtime_dirs.push_back(base + "/" + name);
                    }
                }
                ::closedir(d);
                for (const auto& rt : runtime_dirs) {
                    candidates.push_back(rt + "/native");
                    // Don't hard-code the socket name: list the runtime dir and
                    // take whatever AF_UNIX sockets are in it. Some builds use a
                    // name other than "native", and the liveness check below
                    // discards anything that is not a live daemon anyway.
                    if (DIR* rd = ::opendir(rt.c_str())) {
                        while (const dirent* re = ::readdir(rd)) {
                            const std::string rn = re->d_name;
                            if (rn == "." || rn == ".." || rn == "native") continue;
                            const std::string full = rt + "/" + rn;
                            struct stat rst{};
                            if (::stat(full.c_str(), &rst) == 0 && S_ISSOCK(rst.st_mode)) {
                                candidates.push_back(full);
                            }
                        }
                        ::closedir(rd);
                    }
                }
            }
        }
    }

    for (const auto& c : candidates) {
        if (!path_exists(c)) continue;
        // A socket owned by another user cannot be connected to (PulseAudio is
        // a per-user service and authenticates by uid). Running the client as
        // root against the Termux user's daemon is the common case on Android:
        // report "unavailable" so the client falls straight through to the
        // next backend instead of spending its retry budget on a connection
        // the daemon will refuse.
        struct stat st{};
        if (::stat(c.c_str(), &st) == 0) {
            const uid_t me = ::getuid();
            if (st.st_uid != me && me == 0) {
                LOG_DEBUG("PulsePlayer: found a PulseAudio socket at " << c
                          << " owned by uid " << st.st_uid
                          << ", but this process is root; a per-user daemon will refuse it.");
                continue;
            }
        }
        // The inode exists and is ours - but is anything actually listening?
        // A daemon killed by Android leaves the socket file behind, and that
        // stale inode is what made the client report "reachable" and then sit
        // in a blocking pa_simple_new().
        if (!unix_socket_is_live(c)) {
            LOG_DEBUG("PulsePlayer: " << c << " exists but no daemon is listening on it "
                      "(stale socket from a stopped daemon).");
            continue;
        }
        LOG_DEBUG("PulsePlayer: live daemon socket found at " << c);
        // Hand libpulse this exact socket rather than letting it search: its
        // own lookup does not know about Termux's
        // $HOME/.config/pulse/<machine-id>-runtime layout, so it would refuse
        // a daemon we just verified is alive.
        return "unix:" + c;
    }
    LOG_DEBUG("PulsePlayer: no live daemon socket among " << candidates.size()
              << " candidate path(s):");
    for (const auto& c : candidates) {
        LOG_DEBUG("PulsePlayer:   tried " << c
                  << (path_exists(c) ? " (exists, not listening)" : " (absent)"));
    }
    return std::string();
#endif
}

std::vector<std::string> PulsePlayer::get_available_sinks() {
    std::vector<std::string> sinks;
    const std::string out = run_command("pactl list short sinks 2>/dev/null");
    size_t pos = 0;
    while (pos < out.size()) {
        size_t eol = out.find('\n', pos);
        if (eol == std::string::npos) eol = out.size();
        const std::string line = out.substr(pos, eol - pos);
        pos = eol + 1;
        // "<index>\t<name>\t<module>\t<sample spec>\t<state>"
        const size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        const size_t t2 = line.find('\t', t1 + 1);
        const std::string name = line.substr(t1 + 1, (t2 == std::string::npos ? line.size() : t2) - t1 - 1);
        if (!name.empty()) sinks.push_back(name);
    }
    return sinks;
}

bool PulsePlayer::open(const AudioConfig& config, const std::string& device_name) {
    close();

    config_ = config;
    device_name_ = device_name.empty() ? "pulse" : device_name;
    spec_ = pulse::parse_device_spec(device_name_);
    consecutive_write_failures_ = 0;

#if defined(PULSEAUDIO_ENABLED)
    // Fail fast when no daemon is reachable. Without this an explicit
    // "-d pulse" went straight into pa_simple_new(), which does NOT reliably
    // return an error when there is nothing to talk to: it can block (name
    // lookup, autospawn) until the client's 3 s open watchdog fires, which
    // strands playback on the dummy sink instead of falling through to the
    // next backend. The `default`-name path was already gated by this probe
    // in build_open_strategies(); the explicit device name bypassed it.
    if (!server_available()) {
        LOG_WARN("PulsePlayer: no PulseAudio daemon is reachable (no PULSE_SERVER and no "
                 "usable native socket). Falling through to the next backend.");
        LOG_WARN("  Start one as the NORMAL Termux user (not under su). A plain "
                 "'pulseaudio --start' is often not enough: the daemon exits after "
                 "20 s idle, and on Android it needs an explicit output sink. Use:");
        LOG_WARN("    pulseaudio --start --exit-idle-time=-1 --load=module-sles-sink");
        LOG_WARN("  then confirm with 'pactl info'. If that still times out, run the "
                 "daemon in the foreground to see why it dies:");
        LOG_WARN("    pulseaudio -n --exit-idle-time=-1 --load=module-sles-sink "
                 "--log-target=stderr -vvvv");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!connect_locked()) {
        return false;
    }
    is_open_ = true;
    LOG_INFO("PulsePlayer: Streaming to PulseAudio sink '"
             << (spec_.sink.empty() ? "(daemon default)" : spec_.sink) << "' ("
             << config_.to_string() << ", buffer target " << effective_latency_ms_ << " ms)");
    return true;
#else
    LOG_DEBUG("PulsePlayer: built without libpulse support (PULSEAUDIO_ENABLED undefined)");
    return false;
#endif
}

#if defined(PULSEAUDIO_ENABLED)

bool PulsePlayer::connect_locked() {
    const pa_sample_format_t fmt = to_pa_format(config_.format);
    if (fmt == PA_SAMPLE_INVALID) {
        LOG_WARN("PulsePlayer: unsupported sample format "
                 << std::string(to_string_view(config_.format)));
        return false;
    }
    if (config_.sample_rate == 0 || config_.channels == 0) {
        LOG_WARN("PulsePlayer: invalid stream configuration (" << config_.to_string() << ")");
        return false;
    }

    pa_sample_spec ss{};
    ss.format = fmt;
    ss.rate = config_.sample_rate;
    ss.channels = static_cast<uint8_t>(config_.channels);

    const pulse::BufferAttr wanted = pulse::compute_buffer_attr(
        config_.sample_rate, config_.bytes_per_frame(), config_.frames_per_packet,
        spec_.latency_ms);

    pa_buffer_attr attr{};
    attr.maxlength = wanted.maxlength ? wanted.maxlength : static_cast<uint32_t>(-1);
    attr.tlength = wanted.tlength ? wanted.tlength : static_cast<uint32_t>(-1);
    attr.prebuf = wanted.prebuf ? wanted.prebuf : static_cast<uint32_t>(-1);
    attr.minreq = wanted.minreq ? wanted.minreq : static_cast<uint32_t>(-1);
    // Playback streams ignore fragsize; -1 leaves it to the daemon.
    attr.fragsize = static_cast<uint32_t>(-1);

    const size_t bpf = config_.bytes_per_frame();
    effective_latency_ms_ = (bpf && config_.sample_rate && wanted.tlength)
        ? static_cast<uint32_t>((static_cast<uint64_t>(wanted.tlength) * 1000ULL) /
                                (static_cast<uint64_t>(config_.sample_rate) * bpf))
        : 0;

    int error = 0;
    // Connect to the exact daemon resolve_server_address() validated. Passing
    // nullptr here lets libpulse redo its own discovery, which on Termux
    // misses the daemon's runtime directory entirely.
    const std::string server = resolve_server_address();
    const bool from_env = !server.empty() && server == env_or_empty("PULSE_SERVER");
    LOG_INFO("PulsePlayer: connecting to server '"
             << (server.empty() ? "(libpulse default)" : server) << "'"
             << (from_env ? " (from PULSE_SERVER)" : ""));
    // pa_simple_new() sets PA_STREAM_ADJUST_LATENCY internally, so `tlength`
    // is interpreted as the end-to-end latency we want the daemon to keep.
    pa_simple* s = pa_simple_new(
        server.empty() ? nullptr : server.c_str(),  // explicit server address
        "AudioRouter",                              // application name
        PA_STREAM_PLAYBACK,
        spec_.sink.empty() ? nullptr : spec_.sink.c_str(),
        "Remote PC audio",                          // stream description
        &ss,
        nullptr,                                    // default channel map
        &attr,
        &error);

    if (!s) {
        LOG_WARN("PulsePlayer: pa_simple_new failed for sink '"
                 << (spec_.sink.empty() ? "(default)" : spec_.sink)
                 << "' on server '" << (server.empty() ? "(libpulse default)" : server)
                 << "': " << pa_strerror(error));
        if (from_env) {
            LOG_WARN("  That address came from the PULSE_SERVER environment variable. "
                     "If it is stale, unset it (and re-run) so the daemon's own socket "
                     "is discovered instead: unset PULSE_SERVER");
        }
        return false;
    }

    stream_ = s;
    return true;
}

void PulsePlayer::disconnect_locked() {
    if (!stream_) return;
    auto* s = static_cast<pa_simple*>(stream_);
    stream_ = nullptr;
    // Best effort: push whatever is queued before tearing the stream down, so
    // a graceful close does not clip the tail of the audio.
    int error = 0;
    pa_simple_drain(s, &error);
    pa_simple_free(s);
}

#endif // PULSEAUDIO_ENABLED

void PulsePlayer::close() {
    if (!is_open_.exchange(false)) {
        return;
    }
#if defined(PULSEAUDIO_ENABLED)
    std::lock_guard<std::mutex> lock(mutex_);
    disconnect_locked();
    LOG_INFO("PulsePlayer: Closed PulseAudio stream.");
#endif
}

bool PulsePlayer::is_open() const {
    return is_open_.load();
}

size_t PulsePlayer::write_frames(const void* pcm_data, size_t num_frames) {
    if (!is_open_.load() || !pcm_data || num_frames == 0) return 0;

#if defined(PULSEAUDIO_ENABLED)
    const size_t bpf = config_.bytes_per_frame();
    if (bpf == 0) return 0;

    std::lock_guard<std::mutex> lock(mutex_);

    if (!stream_) {
        // The connection died earlier; rebuild it, rate-limited.
        const uint64_t now = get_time_ms();
        if (now - last_reconnect_ms_ < kReconnectIntervalMs) return 0;
        last_reconnect_ms_ = now;
        if (!connect_locked()) return 0;
        LOG_INFO("PulsePlayer: Reconnected to the PulseAudio daemon.");
        consecutive_write_failures_ = 0;
    }

    int error = 0;
    // pa_simple_write() blocks until the daemon has accepted every byte, so a
    // successful call always consumed the whole chunk. That blocking write is
    // what paces the playback thread against the sink's real clock.
    if (pa_simple_write(static_cast<pa_simple*>(stream_), pcm_data, num_frames * bpf, &error) < 0) {
        ++consecutive_write_failures_;
        const uint64_t now = get_time_ms();
        if (now - last_error_log_ms_ >= kErrorLogIntervalMs) {
            last_error_log_ms_ = now;
            LOG_WARN("PulsePlayer: pa_simple_write failed: " << pa_strerror(error));
        }
        if (consecutive_write_failures_ >= kWriteFailuresBeforeReconnect) {
            // The daemon restarted or the sink disappeared: drop the stream so
            // the next write rebuilds it instead of failing forever.
            LOG_WARN("PulsePlayer: PulseAudio stream broken; will reconnect.");
            disconnect_locked();
            consecutive_write_failures_ = 0;
            last_reconnect_ms_ = now;
        }
        return 0;
    }

    consecutive_write_failures_ = 0;
    return num_frames;
#else
    (void)pcm_data;
    (void)num_frames;
    return 0;
#endif
}

size_t PulsePlayer::get_buffer_delay_frames() const {
    if (!is_open_.load()) return 0;

#if defined(PULSEAUDIO_ENABLED)
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stream_) return 0;
    int error = 0;
    const pa_usec_t latency_us = pa_simple_get_latency(static_cast<pa_simple*>(stream_), &error);
    if (error != 0 || latency_us == static_cast<pa_usec_t>(-1)) return 0;
    return static_cast<size_t>((latency_us * config_.sample_rate) / 1000000ULL);
#else
    return 0;
#endif
}

void PulsePlayer::flush() {
    if (!is_open_.load()) return;
#if defined(PULSEAUDIO_ENABLED)
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stream_) return;
    int error = 0;
    // Drop everything still queued in the daemon (used on reconnect/underrun
    // so stale audio is not replayed ahead of the live stream).
    if (pa_simple_flush(static_cast<pa_simple*>(stream_), &error) < 0) {
        LOG_DEBUG("PulsePlayer: pa_simple_flush failed: " << pa_strerror(error));
    }
#endif
}

std::string PulsePlayer::get_device_name() const {
    if (spec_.sink.empty()) return "pulse";
    return "pulse:" + spec_.sink;
}

} // namespace audiorouter
