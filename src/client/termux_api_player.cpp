#include "termux_api_player.hpp"
#include "android_helpers.hpp"
#include "../common/logger.hpp"
#include "../common/time_util.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

// Floor for the issue target (S - L - lead); a too-short segment can make
// MediaPlayer's prepare() fail. Also used by the host-compiled pure helpers.
constexpr double kMinSegmentContentMs = 200.0;

} // namespace

// The private helpers below exist only on Android builds; on host builds the
// class compiles as a stub (only the pure termux_api:: helpers are live, for
// the unit tests).
#if defined(__ANDROID__)

namespace {

// Default segment length: the backend's inherent end-to-end delay.
constexpr uint32_t kDefaultSegmentMs = 2000;
// User-overridable segment range (termux:<ms>).
constexpr uint32_t kMinSegmentMs = 400;
constexpr uint32_t kMaxSegmentMs = 10000;
// The play for the next segment is issued this far before the outgoing
// segment ends: the media player's reset+start latency then hides inside the
// tail of the outgoing segment (a tiny forward skip) instead of opening a
// silence gap on every boundary.
constexpr uint32_t kIssueLeadMs = 100;
// Startup-latency EMA state.
constexpr double kInitialLatencyMs = 300.0;
constexpr double kMinLatencyMs = 60.0;
constexpr double kMaxLatencyMs = 1500.0;
// The plain `am broadcast` path cannot observe prepare()+start() (its
// pclose() returns when the receiver hands the command to the player
// service), so the measured round trip underestimates the real startup by
// roughly the service handling time. Compensate with a fixed bias.
constexpr double kAmFallbackLatencyBiasMs = 150.0;
// The watchdog restarts playback only when the open segment already holds at
// least this much audio (shorter WAVs can fail prepare()).
constexpr uint32_t kMinPlayableMs = 250;

// termux-api socket protocol timeouts (ms).
constexpr int kSocketConnectTimeoutMs = 800;
constexpr int kSocketAckTimeoutMs = 2000;
constexpr int kInfoTimeoutMs = 1500;
constexpr int kPlayTimeoutMs = 5000;   // accepts cold app-process starts
constexpr int kProbeTimeoutMs = 1500;

// Watchdog cadence and recovery rate limit.
constexpr uint32_t kWatchdogPollMs = 1000;
constexpr uint64_t kMinRecoverIntervalMs = 3000;

// Abstract unix socket the Termux:API app listens on (termux-api.c).
constexpr const char* kApiListenAddress = "com.termux.api://listen";

// --- small helpers ---------------------------------------------------------

std::string trim(std::string s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

bool write_all(int fd, const void* buf, size_t len) {
    const auto* p = static_cast<const uint8_t*>(buf);
    size_t done = 0;
    while (done < len) {
        const ssize_t n = ::write(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

// Reads from fd until EOF (the API closes the socket when the result is
// complete). Returns false on timeout or read error; content is appended to
// *out.
bool read_to_eof(int fd, std::string* out, int timeout_ms) {
    char buf[1024];
    while (true) {
        struct pollfd pfd = {fd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (pr == 0) return false;  // stalled result socket
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return true;  // EOF: result complete
        out->append(buf, static_cast<size_t>(n));
    }
}

int accept_with_timeout(int listen_fd, int timeout_ms) {
    struct pollfd pfd = {listen_fd, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return -1;
    return ::accept(listen_fd, nullptr, nullptr);
}

// Abstract-namespace unix sockets: sun_path[0] == 0 followed by the name
// (matches android.net.LocalSocketAddress Namespace.ABSTRACT).
socklen_t abstract_addr(struct sockaddr_un* addr, const std::string& name) {
    std::memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    std::memcpy(addr->sun_path + 1, name.data(), name.size());
    return static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + 1 + name.size());
}

int make_abstract_listener(const std::string& name) {
    if (name.empty() || name.size() + 1 >= sizeof(sockaddr_un::sun_path)) return -1;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    const socklen_t len = abstract_addr(&addr, name);
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), len) != 0 ||
        ::listen(fd, 1) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

int connect_abstract(const std::string& name, int timeout_ms) {
    if (name.empty() || name.size() + 1 >= sizeof(sockaddr_un::sun_path)) return -1;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    const socklen_t len = abstract_addr(&addr, name);

    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int r = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), len);
    if (r != 0 && errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    struct pollfd pfd = {fd, POLLOUT, 0};
    if (::poll(&pfd, 1, timeout_ms) <= 0) {
        ::close(fd);
        return -1;
    }
    int so_error = 0;
    socklen_t sl = sizeof(so_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &sl) != 0 || so_error != 0) {
        ::close(fd);
        return -1;
    }
    ::fcntl(fd, F_SETFL, flags);
    return fd;
}

// The `am` command. Termux ships its own wrapper at $PREFIX/bin/am (from the
// termux-am package) which sets up the runtime correctly; /system/bin/am is
// the stock fallback; plain "am" is the PATH last resort.
std::string find_am() {
#if defined(__ANDROID__)
    static const std::string resolved = [] {
        const char* candidates[] = {
            "/data/data/com.termux/files/usr/bin/am",
            "/system/bin/am",
        };
        for (const char* c : candidates) {
            if (::access(c, X_OK) == 0) return std::string(c);
        }
        return std::string("am");
    }();
    return resolved;
#else
    return std::string();
#endif
}

// Outcome of one termux-api listen-socket command (mirrors the official
// termux-api binary's protocol):
//   Unreachable: the app never got the command (connect/ack failed) — an
//                `am broadcast` fallback may still reach it;
//   Executed:    the app acked the command and ran the broadcast; the result
//                text follows over the result socket when it arrives in time.
enum class SocketCommandResult { Unreachable, Executed };

// One command over the termux-api listen-socket protocol:
//   1. two listening abstract sockets (result + input addresses),
//   2. connect to com.termux.api://listen (requires the caller's uid to be
//      the Termux app user, so this path only exists without root),
//   3. send the u16-BE length-prefixed command extras,
//   4. read the ack (single NUL byte = accepted),
//   5. accept + read the result from the result listener. For `play` the
//      result is written only after prepare()+start(), so a successful call
//      measures MediaPlayer's real startup latency.
// result_complete (optional) reports whether the result text was actually
// read to EOF — a timed-out result must not be used as a latency sample.
SocketCommandResult try_socket_command(const std::string& action, const std::string& file,
                                       std::string* result_out, int accept_timeout_ms,
                                       bool* result_complete = nullptr) {
#if defined(__ANDROID__)
    static std::atomic<uint64_t> s_counter{0};
    const uint64_t c = s_counter.fetch_add(1, std::memory_order_relaxed);
    const std::string base =
        "audiorouter_tapi_" + std::to_string(static_cast<long>(::getpid())) + "_" +
        std::to_string(static_cast<unsigned long long>(c));
    const std::string in_name = base + "_in";
    const std::string out_name = base + "_out";

    const int in_fd = make_abstract_listener(in_name);
    const int out_fd = make_abstract_listener(out_name);
    if (in_fd < 0 || out_fd < 0) {
        if (in_fd >= 0) ::close(in_fd);
        if (out_fd >= 0) ::close(out_fd);
        return SocketCommandResult::Unreachable;
    }

    const int conn = connect_abstract(kApiListenAddress, kSocketConnectTimeoutMs);
    if (conn < 0) {
        ::close(in_fd);
        ::close(out_fd);
        return SocketCommandResult::Unreachable;
    }

    const std::string cmdline =
        audiorouter::termux_api::build_command_line(action, file, in_name, out_name);
    if (cmdline.size() > 0xFFFFu) {
        ::close(conn);
        ::close(in_fd);
        ::close(out_fd);
        return SocketCommandResult::Unreachable;
    }
    const uint16_t be_len = htons(static_cast<uint16_t>(cmdline.size()));
    if (!write_all(conn, &be_len, sizeof(be_len)) ||
        !write_all(conn, cmdline.data(), cmdline.size())) {
        ::close(conn);
        ::close(in_fd);
        ::close(out_fd);
        return SocketCommandResult::Unreachable;
    }

    // Ack: one NUL byte means the broadcast was delivered; anything else is
    // an error message; EOF means the app dropped the connection (wrong uid).
    std::string ack;
    const bool acked =
        read_to_eof(conn, &ack, kSocketAckTimeoutMs) && ack.size() == 1 && ack[0] == '\0';
    ::close(conn);
    if (!acked) {
        ::close(in_fd);
        ::close(out_fd);
        return SocketCommandResult::Unreachable;
    }

    // The command ran; the result text is best effort (a timed-out result
    // must NOT be retried via am — the command was already executed).
    std::string result;
    bool complete = false;
    const int acc = accept_with_timeout(out_fd, accept_timeout_ms);
    if (acc >= 0) {
        complete = read_to_eof(acc, &result, accept_timeout_ms);
        ::close(acc);
    }
    ::close(in_fd);
    ::close(out_fd);
    if (result_out != nullptr) *result_out = result;
    if (result_complete != nullptr) *result_complete = complete;
    return SocketCommandResult::Executed;
#else
    (void)action;
    (void)file;
    (void)result_out;
    (void)accept_timeout_ms;
    if (result_complete != nullptr) *result_complete = false;
    return SocketCommandResult::Unreachable;
#endif
}

// Plain `am broadcast` fallback (works on Termux:API versions without the
// listen socket; command results are only visible in its output on older app
// versions). Returns the captured output.
bool send_am_broadcast(const std::string& action, const std::string& file,
                       std::string* result_out) {
#if defined(__ANDROID__)
    const std::string am = find_am();
    const std::string shell = am +
        " broadcast --user 0 -n com.termux.api/.TermuxApiReceiver " +
        audiorouter::termux_api::build_command_line(action, file, "", "") + " 2>&1";
    std::string captured;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(audiorouter::AndroidHelpers::subprocess_mutex());
        if (FILE* f = ::popen(shell.c_str(), "r")) {
            char buf[512];
            while (::fgets(buf, static_cast<int>(sizeof(buf)), f) != nullptr) {
                captured += buf;
            }
            ok = (::pclose(f) == 0);
        }
    }
    if (result_out != nullptr) *result_out = trim(captured);
    return ok;
#else
    (void)action;
    (void)file;
    (void)result_out;
    return false;
#endif
}

// Preferred command path with result text: socket protocol first, plain am
// broadcast fallback (only when the socket path could not reach the app at
// all). Also records which path succeeded. result_complete (optional)
// reports whether the command duration observed by the caller is a full
// prepare()+start() measurement (socket path with the result read back, or
// the am fallback's pclose) — a lost socket result must not be used as a
// latency sample.
bool send_media_command(std::atomic<bool>* socket_protocol, const std::string& action,
                        const std::string& file, std::string* result_out, int accept_timeout_ms,
                        bool* result_complete = nullptr) {
#if defined(__ANDROID__)
    std::string result;
    bool complete = false;
    const SocketCommandResult sr =
        try_socket_command(action, file, &result, accept_timeout_ms, &complete);
    if (sr == SocketCommandResult::Executed) {
        socket_protocol->store(true);
        if (result_out != nullptr) *result_out = result;
        if (result_complete != nullptr) *result_complete = complete;
        return true;
    }
    socket_protocol->store(false);
    const bool ok = send_am_broadcast(action, file, result_out);
    if (result_complete != nullptr) *result_complete = ok;
    return ok;
#else
    (void)socket_protocol;
    (void)action;
    (void)file;
    (void)result_out;
    (void)accept_timeout_ms;
    if (result_complete != nullptr) *result_complete = false;
    return false;
#endif
}

// Converts S24LE / S32LE interleaved input to S16LE (the WAV on disk is
// always PCM16; FLOAT32 input is written as float WAV instead).
void convert_to_s16le(const uint8_t* src, size_t frames, uint16_t channels,
                      audiorouter::AudioSampleFormat fmt, std::vector<uint8_t>& out) {
    const size_t samples = frames * static_cast<size_t>(channels);
    out.resize(samples * 2);
    auto* dst = reinterpret_cast<int16_t*>(out.data());
    if (fmt == audiorouter::AudioSampleFormat::PCM_S32LE) {
        for (size_t i = 0; i < samples; ++i) {
            const uint32_t b0 = src[i * 4];
            const uint32_t b1 = src[i * 4 + 1];
            const uint32_t b2 = src[i * 4 + 2];
            const uint32_t b3 = src[i * 4 + 3];
            const int32_t v = static_cast<int32_t>(b0 | (b1 << 8) | (b2 << 16) | (b3 << 24));
            dst[i] = static_cast<int16_t>(v >> 16);
        }
    } else {  // PCM_S24LE
        for (size_t i = 0; i < samples; ++i) {
            const uint32_t b0 = src[i * 3];
            const uint32_t b1 = src[i * 3 + 1];
            const uint32_t b2 = src[i * 3 + 2];
            int32_t v = static_cast<int32_t>(b0 | (b1 << 8) | (b2 << 16));
            if ((v & 0x800000) != 0) v |= ~0xFFFFFF;  // sign-extend 24 -> 32
            dst[i] = static_cast<int16_t>(v >> 8);
        }
    }
}

void store_le32(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v);
    dst[1] = static_cast<uint8_t>(v >> 8);
    dst[2] = static_cast<uint8_t>(v >> 16);
    dst[3] = static_cast<uint8_t>(v >> 24);
}

} // namespace

#endif // __ANDROID__

namespace audiorouter {

namespace termux_api {

std::vector<uint8_t> build_wav_header(uint32_t sample_rate, uint16_t channels,
                                      bool float_format, uint32_t riff_sz,
                                      uint32_t data_sz) {
    std::vector<uint8_t> h(44, 0);
    auto put32 = [&h](size_t off, uint32_t v) {
        h[off] = static_cast<uint8_t>(v);
        h[off + 1] = static_cast<uint8_t>(v >> 8);
        h[off + 2] = static_cast<uint8_t>(v >> 16);
        h[off + 3] = static_cast<uint8_t>(v >> 24);
    };
    auto put16 = [&h](size_t off, uint16_t v) {
        h[off] = static_cast<uint8_t>(v);
        h[off + 1] = static_cast<uint8_t>(v >> 8);
    };

    const uint32_t bytes_per_sample = float_format ? 4u : 2u;
    const uint32_t block_align = static_cast<uint32_t>(channels) * bytes_per_sample;

    std::memcpy(&h[0], "RIFF", 4);
    put32(4, riff_sz);
    std::memcpy(&h[8], "WAVE", 4);
    std::memcpy(&h[12], "fmt ", 4);
    put32(16, 16);  // fmt chunk size (no extension)
    put16(20, float_format ? 3u : 1u);  // IEEE float / PCM
    put16(22, channels);
    put32(24, sample_rate);
    put32(28, sample_rate * block_align);
    put16(32, static_cast<uint16_t>(block_align));
    put16(34, float_format ? 32u : 16u);
    std::memcpy(&h[36], "data", 4);
    put32(40, data_sz);
    return h;
}

bool segment_ready(const SegmentClock& clock, uint64_t now_ms, uint32_t segment_ms,
                   double latency_est_ms, uint32_t issue_lead_ms) {
    const uint32_t rate = clock.sample_rate > 0 ? clock.sample_rate : 48000;
    double target_ms = static_cast<double>(segment_ms) - latency_est_ms -
                       static_cast<double>(issue_lead_ms);
    if (target_ms < kMinSegmentContentMs) target_ms = kMinSegmentContentMs;

    const double content_ms =
        static_cast<double>(clock.frames_written) * 1000.0 / static_cast<double>(rate);
    if (content_ms < target_ms) return false;

    const double wall_ms = (now_ms >= clock.seg_start_wall_ms)
                               ? static_cast<double>(now_ms - clock.seg_start_wall_ms)
                               : 0.0;
    return wall_ms >= target_ms;
}

std::string build_command_line(const std::string& action, const std::string& file,
                               const std::string& in_addr, const std::string& out_addr) {
    std::string cmd;
    if (!in_addr.empty()) cmd += "--es socket_input \"" + in_addr + "\" ";
    if (!out_addr.empty()) cmd += "--es socket_output \"" + out_addr + "\" ";
    // NOTE: the value must be quoted — the app's parser only extracts
    // quoted --es values, and any leftover token fails the command.
    cmd += "--es api_method \"MediaPlayer\" -a " + action;
    if (!file.empty()) cmd += " --es file \"" + file + "\"";
    return cmd;
}

} // namespace termux_api

TermuxApiPlayer::TermuxApiPlayer() = default;

TermuxApiPlayer::~TermuxApiPlayer() {
    close();
}

bool TermuxApiPlayer::open(const AudioConfig& config, const std::string& device_name) {
#if defined(__ANDROID__)
    if (is_open_.load()) close();

    config_ = config;
    if (config_.sample_rate == 0) config_.sample_rate = 48000;
    if (config_.channels == 0) config_.channels = 2;

    segment_ms_ = kDefaultSegmentMs;
    device_name_ = device_name.empty() ? "termux" : device_name;
    if (device_name_.rfind("termux:", 0) == 0) {
        const std::string rest = device_name_.substr(7);
        if (!rest.empty()) {
            try {
                const long ms = std::stol(rest);
                const long clamped = std::clamp(ms, static_cast<long>(kMinSegmentMs),
                                                static_cast<long>(kMaxSegmentMs));
                segment_ms_ = static_cast<uint32_t>(clamped);
                if (clamped != ms) {
                    LOG_WARN("TermuxApiPlayer: segment length " << ms << " ms out of range ["
                             << kMinSegmentMs << ", " << kMaxSegmentMs << "]; clamped to "
                             << segment_ms_ << " ms");
                }
            } catch (const std::exception&) {
                LOG_WARN("TermuxApiPlayer: invalid segment length '" << rest
                        << "'; using " << kDefaultSegmentMs << " ms");
            }
        }
    }

    if (!resolve_cache_dir()) return false;
    cleanup_stale_segments();

    // Warm the API app process (and start its socket listener) before the
    // probe: a cold com.termux.api takes hundreds of ms to come up, which
    // would otherwise cost us the first segment. Best effort.
    if (geteuid() != 0) {
        std::lock_guard<std::mutex> lock(AndroidHelpers::subprocess_mutex());
        (void)::system((find_am() +
                        " startservice -n com.termux.api/.KeepAliveService 2>/dev/null")
                           .c_str());
    }

    // Preflight: the Termux:API app must answer a media_player command.
    std::string probe;
    socket_protocol_.store(false);
    if (!send_media_command(&socket_protocol_, "info", "", &probe, kProbeTimeoutMs)) {
        LOG_ERROR("TermuxApiPlayer: the Termux:API app did not answer a media_player "
                  "command (is com.termux.api installed?)");
        LOG_ERROR("TermuxApiPlayer: install Termux:API from F-Droid (the "
                  "termux-media-player API), then retry with -d termux");
        return false;
    }
    if (!probe.empty()) LOG_DEBUG("TermuxApiPlayer: probe result: " << probe);

    latency_est_ms_ = kInitialLatencyMs;
    issue_wall_ms_ = 0;
    issued_frames_ = 0;
    issued_path_prev_.clear();
    seg_fd_ = -1;
    seg_path_.clear();
    seg_frames_ = 0;
    seg_index_ = 0;
    seg_start_wall_ms_ = 0;
    last_recover_ms_ = 0;

    is_open_.store(true);
    stop_watchdog_.store(false);
    // Always spawn the watchdog: it only acts while the socket protocol is
    // available, and it (re)discovers that protocol on its own if the app's
    // listener came up after open().
    watchdog_thread_ = std::thread(&TermuxApiPlayer::watchdog_loop, this);
    LOG_INFO("TermuxApiPlayer: Termux:API media player ready (" << segment_ms_
             << " ms segments, " << (socket_protocol_.load() ? "socket protocol" : "am broadcast")
             << ", cache " << cache_dir_ << ")");
    return true;
#else
    (void)config;
    (void)device_name;
    LOG_INFO("TermuxApiPlayer: Termux:API playback not available on this platform");
    return false;
#endif
}

bool TermuxApiPlayer::resolve_cache_dir() {
#if defined(__ANDROID__)
    std::string base;
    const bool root = (geteuid() == 0);
    if (!root) {
        const char* home = std::getenv("HOME");
        if (home != nullptr && home[0] != '\0' && std::strcmp(home, "/") != 0) {
            base = home;
        }
        if (base.empty()) {
            const char* override = std::getenv("AUDIOROUTER_TERMUX_HOME");
            base = (override != nullptr && override[0] != '\0')
                       ? override
                       : "/data/data/com.termux/files/home";
        }
    } else {
        // Root: still prefer the Termux home — com.termux.api can only read
        // files inside the shared Termux sandbox.
        std::string termux_home;
        if (AndroidHelpers::termux_user(nullptr, nullptr, &termux_home)) base = termux_home;
    }
    if (base.empty()) {
        base = "/data/local/tmp";
        LOG_WARN("TermuxApiPlayer: Termux home not found; falling back to " << base
                 << " — com.termux.api will NOT be able to read the segments (running as the "
                    "Termux app user is required)");
    }

    cache_dir_ = base + "/.audiorouter";
    if (::mkdir(cache_dir_.c_str(), 0777) != 0 && errno != EEXIST) {
        LOG_ERROR("TermuxApiPlayer: cannot create cache dir " << cache_dir_ << ": "
                 << std::strerror(errno));
        return false;
    }
    ::chmod(cache_dir_.c_str(), 0777);
    if (root) {
        // Best effort: give the directory the app-data SELinux label so the
        // API app can read root-created files. Running as the Termux user is
        // the supported path for this backend (see termux_run.sh).
        std::lock_guard<std::mutex> lock(AndroidHelpers::subprocess_mutex());
        (void)::system(("restorecon -RF " + cache_dir_ + " 2>/dev/null").c_str());
        LOG_WARN("TermuxApiPlayer: running as root — segments are written to " << cache_dir_
                 << "; the Termux:API app must be able to read them. Running as the Termux "
                    "app user (no su) is recommended for -d termux.");
    }
    return true;
#else
    return false;
#endif
}

void TermuxApiPlayer::cleanup_stale_segments() {
    DIR* dir = ::opendir(cache_dir_.c_str());
    if (dir == nullptr) return;
    struct dirent* entry;
    while ((entry = ::readdir(dir)) != nullptr) {
        const std::string name = entry->d_name;
        if (name.rfind("seg_", 0) != 0) continue;
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".wav") != 0) continue;
        ::unlink((cache_dir_ + "/" + name).c_str());
    }
    ::closedir(dir);
}

bool TermuxApiPlayer::ensure_segment_locked() {
    if (seg_fd_ >= 0) return true;
    return open_next_segment_locked();
}

bool TermuxApiPlayer::open_next_segment_locked() {
    if (seg_fd_ >= 0) {
        ::close(seg_fd_);
        seg_fd_ = -1;
    }
    ++seg_index_;
    seg_path_ = cache_dir_ + "/seg_" + std::to_string(seg_index_) + ".wav";
    seg_fd_ = ::open(seg_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (seg_fd_ < 0) {
        LOG_ERROR("TermuxApiPlayer: cannot create " << seg_path_ << ": " << std::strerror(errno));
        return false;
    }
    // The Termux:API app reads the file under its own uid (shared user id
    // with Termux), so keep the mode bits permissive.
    ::fchmod(seg_fd_, 0666);

    const bool is_float = config_.format == AudioSampleFormat::PCM_FLOAT32LE;
    const uint32_t rate = config_.sample_rate;
    const auto header = termux_api::build_wav_header(rate, config_.channels, is_float,
                                                     0xFFFFFFFFu, 0xFFFFFFFFu);
    if (::write(seg_fd_, header.data(), header.size()) !=
        static_cast<ssize_t>(header.size())) {
        LOG_ERROR("TermuxApiPlayer: failed to write WAV header to " << seg_path_ << ": "
                  << std::strerror(errno));
        ::close(seg_fd_);
        seg_fd_ = -1;
        ::unlink(seg_path_.c_str());
        seg_path_.clear();
        return false;
    }
    seg_frames_ = 0;
    seg_start_wall_ms_ = get_time_ms();
    return true;
}

void TermuxApiPlayer::discard_segment_locked() {
    if (seg_fd_ >= 0) {
        ::close(seg_fd_);
        seg_fd_ = -1;
    }
    if (!seg_path_.empty()) {
        ::unlink(seg_path_.c_str());
        seg_path_.clear();
    }
    seg_frames_ = 0;
    seg_start_wall_ms_ = get_time_ms();  // re-anchor at live audio
}

void TermuxApiPlayer::issue_segment_locked() {
#if defined(__ANDROID__)
    // Patch exact sizes: MediaPlayer's duration comes from the header, and
    // the track then ends exactly at the segment's EOF.
    const size_t bytes_per_frame =
        (config_.format == AudioSampleFormat::PCM_FLOAT32LE)
            ? config_.bytes_per_frame()
            : 2 * static_cast<size_t>(config_.channels);
    const uint32_t data_bytes = static_cast<uint32_t>(seg_frames_ * bytes_per_frame);
    uint8_t le[4];
    store_le32(le, 36u + data_bytes);
    (void)::pwrite(seg_fd_, le, sizeof(le), 4);   // riff size
    store_le32(le, data_bytes);
    (void)::pwrite(seg_fd_, le, sizeof(le), 40);  // data chunk size

    const std::string path = seg_path_;
    const size_t frames = seg_frames_;

    const uint64_t t0 = get_time_ms();
    std::string result;
    bool result_complete = false;
    const bool ok = send_media_command(&socket_protocol_, "play", path, &result, kPlayTimeoutMs,
                                       &result_complete);
    const uint64_t elapsed = get_time_ms() - t0;

    if (ok) {
        // EMA on the measured startup: the socket protocol's result arrives
        // only after prepare()+start(), so this includes MediaPlayer's real
        // startup (cold app processes included); the am fallback gets a
        // fixed bias instead. A lost result (accept timeout) is not a valid
        // sample - keep the previous estimate.
        if (result_complete) {
            const double bias = socket_protocol_.load() ? 0.0 : kAmFallbackLatencyBiasMs;
            const double sample = static_cast<double>(elapsed) + bias;
            latency_est_ms_ = std::clamp(latency_est_ms_ * 0.65 + sample * 0.35,
                                         kMinLatencyMs, kMaxLatencyMs);
        } else {
            LOG_DEBUG("TermuxApiPlayer: play result lost; keeping the previous latency "
                      "estimate");
        }
        issue_wall_ms_ = get_time_ms();
        issued_frames_ = frames;
        // The player service closed the outgoing track's file when it reset
        // the player for this play, so its segment file can go now.
        if (!issued_path_prev_.empty()) {
            ::unlink(issued_path_prev_.c_str());
            issued_path_prev_.clear();
        }
        issued_path_prev_ = path;
        if (!result.empty()) {
            LOG_DEBUG("TermuxApiPlayer: media player: " << trim(result) << " (startup "
                     << elapsed << " ms)");
        }
        open_next_segment_locked();
        return;
    }

    LOG_WARN("TermuxApiPlayer: play command failed after " << elapsed << " ms"
             << (result.empty() ? std::string() : ": " + trim(result)));
    // The segment's audio has never been heard: discard it and resume at
    // live audio (the same stale-audio policy as the FIFO backends). The
    // next boundary retries with a fresh segment.
    discard_segment_locked();
#endif
}

void TermuxApiPlayer::watchdog_loop() {
#if defined(__ANDROID__)
    while (!stop_watchdog_.load()) {
        sleep_ms(kWatchdogPollMs);
        if (stop_watchdog_.load() || !is_open_.load()) break;

        // Peek at the player state. This also (re)discovers the socket
        // protocol when the app's listener only came up after open().
        std::string info;
        if (try_socket_command("info", "", &info, kInfoTimeoutMs) !=
            SocketCommandResult::Executed) {
            continue;  // socket path unavailable: nothing actionable without results
        }
        socket_protocol_.store(true);

        const bool paused = info.find("Paused") != std::string::npos;
        const bool no_track = info.find("No track") != std::string::npos;
        if (!paused && !no_track) continue;

        std::lock_guard<std::mutex> lock(io_mutex_);
        if (!is_open_.load() || stop_watchdog_.load()) break;
        if (issued_frames_ == 0) continue;  // nothing was handed to the player yet

        // Re-query under the lock so the decision uses fresh state (a play
        // issued concurrently could have made the peek above stale).
        std::string fresh;
        if (try_socket_command("info", "", &fresh, kInfoTimeoutMs) !=
            SocketCommandResult::Executed) {
            continue;
        }
        const bool fresh_paused = fresh.find("Paused") != std::string::npos;
        const bool fresh_no_track = fresh.find("No track") != std::string::npos;

        if (fresh_paused) {
            LOG_WARN("TermuxApiPlayer: media player paused (audio focus?); resuming");
            std::string result;
            if (try_socket_command("resume", "", &result, kInfoTimeoutMs) !=
                SocketCommandResult::Executed) {
                LOG_WARN("TermuxApiPlayer: resume command did not reach the app");
            }
            continue;
        }
        if (!fresh_no_track) continue;

        // The player died mid-segment (app killed, error, ...). Restart at
        // live audio from the open segment, rate-limited.
        const uint64_t now_ms = get_time_ms();
        if (now_ms - last_recover_ms_ < kMinRecoverIntervalMs) continue;
        last_recover_ms_ = now_ms;

        const uint32_t rate = config_.sample_rate;
        const size_t min_frames = static_cast<size_t>(kMinPlayableMs) * rate / 1000;
        if (seg_fd_ < 0 || seg_frames_ < min_frames) {
            // Nothing buffered (stalled network): clear the issued
            // bookkeeping so the normal boundary restarts playback.
            issue_wall_ms_ = 0;
            issued_frames_ = 0;
            continue;
        }
        LOG_WARN("TermuxApiPlayer: media player stopped mid-stream; restarting playback at "
                 "live audio");
        recovering_.store(true);
        issue_segment_locked();
        recovering_.store(false);
    }
#endif
}

void TermuxApiPlayer::close() {
#if defined(__ANDROID__)
    stop_watchdog_.store(true);
    if (watchdog_thread_.joinable()) watchdog_thread_.join();

    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!is_open_.exchange(false)) return;

    // Politely stop whatever is playing (fire-and-forget am; the track would
    // also end by itself at the segment EOF).
    if (issued_frames_ > 0) {
        std::string ignored;
        (void)send_am_broadcast("stop", "", &ignored);
    }
    if (seg_fd_ >= 0) {
        ::close(seg_fd_);
        seg_fd_ = -1;
    }
    if (!seg_path_.empty()) {
        ::unlink(seg_path_.c_str());
        seg_path_.clear();
    }
    if (!issued_path_prev_.empty()) {
        ::unlink(issued_path_prev_.c_str());
        issued_path_prev_.clear();
    }
    seg_frames_ = 0;
    issued_frames_ = 0;
    issue_wall_ms_ = 0;
    LOG_INFO("TermuxApiPlayer: Termux:API playback stopped");
#else
    is_open_.store(false);
#endif
}

bool TermuxApiPlayer::is_open() const {
    return is_open_.load();
}

bool TermuxApiPlayer::is_supported() {
#if defined(__ANDROID__)
    return true;
#else
    return false;
#endif
}

bool TermuxApiPlayer::api_available() {
#if defined(__ANDROID__)
    std::atomic<bool> protocol{false};
    std::string result;
    return send_media_command(&protocol, "info", "", &result, kProbeTimeoutMs);
#else
    return false;
#endif
}

size_t TermuxApiPlayer::write_frames(const void* pcm_data, size_t num_frames) {
    if (pcm_data == nullptr || num_frames == 0) return 0;
#if defined(__ANDROID__)
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!is_open_.load()) return 0;
    if (recovering_.load()) return 0;  // watchdog restart in progress: the jitter buffer absorbs

    const uint32_t rate = config_.sample_rate;
    const size_t in_bytes_per_frame = config_.bytes_per_frame();
    if (in_bytes_per_frame == 0) return 0;

    // The WAV on disk is PCM16 interleaved (or IEEE float); S24/S32 input is
    // converted first.
    const uint8_t* data = static_cast<const uint8_t*>(pcm_data);
    if (config_.format == AudioSampleFormat::PCM_S24LE ||
        config_.format == AudioSampleFormat::PCM_S32LE) {
        convert_to_s16le(data, num_frames, config_.channels, config_.format, convert_buf_);
        data = convert_buf_.data();
    }
    const size_t bytes_per_frame =
        (config_.format == AudioSampleFormat::PCM_FLOAT32LE)
            ? in_bytes_per_frame
            : 2 * static_cast<size_t>(config_.channels);

    if (!ensure_segment_locked()) return 0;

    const size_t total_bytes = num_frames * bytes_per_frame;
    size_t written = 0;
    while (written < total_bytes) {
        const ssize_t n = ::write(seg_fd_, data + written, total_bytes - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        LOG_ERROR("TermuxApiPlayer: segment write failed (" << std::strerror(errno)
                 << "); discarding the segment");
        discard_segment_locked();
        return 0;
    }
    seg_frames_ += num_frames;

    // Hand the segment to the media player once it holds enough audio AND
    // the wall clock agrees (gapless chaining; see termux_api::segment_ready).
    termux_api::SegmentClock clock{seg_start_wall_ms_, seg_frames_, rate};
    if (termux_api::segment_ready(clock, get_time_ms(), segment_ms_, latency_est_ms_,
                                  kIssueLeadMs)) {
        issue_segment_locked();
    }
    return num_frames;
#else
    (void)pcm_data;
    (void)num_frames;
    return 0;
#endif
}

size_t TermuxApiPlayer::get_buffer_delay_frames() const {
    if (!is_open_.load()) return 0;
    std::lock_guard<std::mutex> lock(io_mutex_);
    const uint32_t rate = config_.sample_rate > 0 ? config_.sample_rate : 48000;

    // Frames recorded into the open (not yet handed over) segment.
    const size_t unissued = seg_frames_;

    // Remainder of the issued segment that has not been heard yet, estimated
    // from the wall clock (MediaPlayer plays at real time from issue+start).
    size_t issued_remaining = 0;
    if (issued_frames_ > 0 && issue_wall_ms_ > 0) {
        const uint64_t now_ms = get_time_ms();
        if (now_ms > issue_wall_ms_) {
            const double played_ms =
                static_cast<double>(now_ms - issue_wall_ms_) - latency_est_ms_;
            if (played_ms > 0) {
                const size_t played = static_cast<size_t>(played_ms * rate / 1000.0);
                issued_remaining = played < issued_frames_ ? issued_frames_ - played : 0;
            } else {
                issued_remaining = issued_frames_;
            }
        }
    }
    // MediaPlayer's own buffered lookahead (~ its startup latency).
    const size_t in_player = static_cast<size_t>(latency_est_ms_ * rate / 1000.0);
    return unissued + issued_remaining + in_player;
}

void TermuxApiPlayer::flush() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!is_open_.load()) return;
    // Drop whatever is buffered in the open segment; playback resumes at
    // live audio on the next boundary (same stale-audio policy as the FIFO
    // backends).
    if (seg_frames_ > 0) {
        discard_segment_locked();
        LOG_INFO("TermuxApiPlayer: discarded buffered segment audio (resync to live)");
    }
}

std::string TermuxApiPlayer::get_device_name() const {
    return device_name_;
}

} // namespace audiorouter
