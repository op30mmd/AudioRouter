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
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#if defined(__ANDROID__) && defined(__BIONIC__)
#include <sys/system_properties.h>
#endif

namespace {

// Floor for the issue target (S - L - lead); a too-short segment can make
// MediaPlayer's prepare() fail. Also used by the host-compiled pure helpers.
constexpr double kMinSegmentContentMs = 200.0;
// The issue cadence must stay this far above the command latency so the
// single issuer thread finishes each play before the next one is due (this
// is what keeps playback continuous when commands are slow).
constexpr double kIssueMarginMs = 200.0;

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
// Startup-latency EMA state. The upper bound must comfortably exceed the
// am-broadcast latency on a frozen app, otherwise the issuer cannot keep up.
constexpr double kInitialLatencyMs = 300.0;
constexpr double kMinLatencyMs = 60.0;
constexpr double kMaxLatencyMs = 4000.0;
// Blind mode only (no result channel at all): the am round trip cannot
// observe prepare()+start(), so the fixed estimate gets a small bias on top
// of the measured am duration.
constexpr double kBlindSeedBiasMs = 150.0;

// Segment file pool: a small fixed set of inodes created (and, as root,
// SELinux-labeled) before streaming starts, then recycled with O_TRUNC so
// the labels survive the privilege drop and the app can always read them.
// 8 x 2 s = 16 s of reuse distance - the app finished reading long before a
// slot is reused.
constexpr int kPoolSize = 8;

// termux-api socket protocol timeouts (ms).
constexpr int kSocketConnectTimeoutMs = 400;
constexpr int kSocketAckTimeoutMs = 800;
constexpr int kInfoTimeoutMs = 1500;
constexpr int kPlayTimeoutMs = 5000;   // accepts cold app-process starts
constexpr int kProbeTimeoutMs = 1500;

// Watchdog cadence.
constexpr uint32_t kWatchdogPollMs = 1000;

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

int android_sdk_int() {
#if defined(__ANDROID__) && defined(__BIONIC__)
    char buf[PROP_VALUE_MAX];
    if (__system_property_get("ro.build.version.sdk", buf) > 0) {
        const int v = std::atoi(buf);
        if (v > 0) return v;
    }
    return 0;
#else
    return 0;
#endif
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

// The Termux app's SELinux context, captured (as root) by
// prepare_cache_as_root() and loaded by the player at open(). When this
// client runs in a root-shell domain (su), the confined app cannot connect
// to sockets created there; labeling our listener sockets with the app's own
// context fixes that. Empty = no labeling (plain app-user run: our sockets
// already carry the right domain).
std::string g_sockcreate_context;

// Creates a unix socket, optionally labeled with g_sockcreate_context via
// setsockcreatecon_raw (libselinux). Falls back to a plain socket when the
// labeling is unavailable or the current domain may not create such sockets.
int create_listener_socket() {
    static int (*setsockcreatecon_raw)(const char*) = []() -> int (*)(const char*) {
        void* h = ::dlopen("libselinux.so", RTLD_NOW);
        if (h == nullptr) h = ::dlopen("/system/lib64/libselinux.so", RTLD_NOW);
        if (h == nullptr) return nullptr;
        auto* fn = reinterpret_cast<int (*)(const char*)>(
            ::dlsym(h, "setsockcreatecon_raw"));
        if (fn == nullptr) {
            fn = reinterpret_cast<int (*)(const char*)>(::dlsym(h, "setsockcreatecon"));
        }
        return fn;
    }();

    if (!g_sockcreate_context.empty() && setsockcreatecon_raw != nullptr &&
        setsockcreatecon_raw(g_sockcreate_context.c_str()) == 0) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        setsockcreatecon_raw(nullptr);
        if (fd >= 0) return fd;
        // The domain may not be allowed to create sockets with that context
        // (EACCES); fall through to a plain socket.
    }
    return ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
}

// Reads the context file written by prepare_cache_as_root().
std::string load_app_context(const std::string& dir) {
    const std::string path = dir + "/app_context";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    char buf[256];
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return "";
    buf[n] = '\0';
    std::string ctx(buf);
    while (!ctx.empty() && (ctx.back() == '\n' || ctx.back() == '\r')) ctx.pop_back();
    return ctx;
}

// Finds the runtime SELinux context of a process running as app_uid (the
// Termux app user), e.g. u:r:untrusted_app:s0:c512,c768. Requires root to
// read /proc/<pid>/attr/current of another domain.
std::string find_app_context(uid_t app_uid) {
    DIR* dir = ::opendir("/proc");
    if (dir == nullptr) return "";
    std::string ctx;
    struct dirent* e;
    while ((e = ::readdir(dir)) != nullptr) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        const std::string base = std::string("/proc/") + e->d_name;
        bool uid_match = false;
        if (FILE* f = ::fopen((base + "/status").c_str(), "r")) {
            char line[256];
            while (::fgets(line, sizeof(line), f) != nullptr) {
                if (std::strncmp(line, "Uid:", 4) == 0) {
                    uid_match = (static_cast<unsigned long>(app_uid) ==
                                 std::strtoul(line + 4, nullptr, 10));
                    break;
                }
            }
            ::fclose(f);
        }
        if (!uid_match) continue;
        const int fd = ::open((base + "/attr/current").c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char buf[256];
        const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';
        std::string c(buf);
        while (!c.empty() && (c.back() == '\n' || c.back() == '\r')) c.pop_back();
        if (!c.empty()) {
            ctx = c;
            break;
        }
    }
    ::closedir(dir);
    return ctx;
}

int make_abstract_listener(const std::string& name) {
    if (name.empty() || name.size() + 1 >= sizeof(sockaddr_un::sun_path)) return -1;
    const int fd = create_listener_socket();
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

// Plain `am broadcast` (no result sockets): delivery only, the am command's
// own chatter comes back as the "result". Used by close()'s fire-and-forget
// stop and by blind mode.
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

// Outcome of one media command.
struct MediaCommandOutcome {
    bool delivered = false;        // the app got the command
    bool result_received = false;  // the app wrote its result text back
    bool socket_path = false;      // listen-socket protocol (vs am broadcast)
    std::string result;
    // Duration of the successful delivery path. With a result received this
    // is the full command time incl. prepare()+start() — a real latency
    // sample. Without one it is just the am round trip.
    uint64_t duration_ms = 0;
};

// `am broadcast` carrying the client's own result-socket extras — the path
// the official termux-api binary uses on Android 14+, where the app's own
// listen socket freezes but the client's listener sockets work fine. The
// app writes its result (after prepare()+start()) to socket_output; we wait
// for it after `am` returns.
MediaCommandOutcome send_am_socket_broadcast(const std::string& action,
                                             const std::string& file,
                                             int accept_timeout_ms) {
    MediaCommandOutcome out;
#if defined(__ANDROID__)
    static std::atomic<uint64_t> s_am_counter{0};
    const uint64_t c = s_am_counter.fetch_add(1, std::memory_order_relaxed);
    const std::string base =
        "audiorouter_tapi_am_" + std::to_string(static_cast<long>(::getpid())) + "_" +
        std::to_string(static_cast<unsigned long long>(c));
    const std::string in_name = base + "_in";
    const std::string out_name = base + "_out";

    const int in_fd = make_abstract_listener(in_name);
    const int out_fd = make_abstract_listener(out_name);
    if (in_fd < 0 || out_fd < 0) {
        if (in_fd >= 0) ::close(in_fd);
        if (out_fd >= 0) ::close(out_fd);
        // No listener sockets available: degrade to delivery-only am.
        std::string txt;
        const uint64_t t0 = audiorouter::get_time_ms();
        out.delivered = send_am_broadcast(action, file, &txt);
        out.duration_ms = audiorouter::get_time_ms() - t0;
        out.result = txt;
        return out;
    }

    const std::string shell = find_am() +
        " broadcast --user 0 -n com.termux.api/.TermuxApiReceiver " +
        audiorouter::termux_api::build_command_line(action, file, in_name, out_name) +
        " 2>&1";

    std::string am_stdout;
    int rc = -1;
    const uint64_t t0 = audiorouter::get_time_ms();
    {
        std::lock_guard<std::mutex> lock(audiorouter::AndroidHelpers::subprocess_mutex());
        if (FILE* f = ::popen(shell.c_str(), "r")) {
            char buf[512];
            while (::fgets(buf, static_cast<int>(sizeof(buf)), f) != nullptr) {
                am_stdout += buf;
            }
            rc = ::pclose(f);
        }
    }
    const uint64_t am_ms = audiorouter::get_time_ms() - t0;
    if (rc != 0) {
        ::close(in_fd);
        ::close(out_fd);
        out.result = trim(am_stdout);
        return out;  // not delivered
    }
    out.delivered = true;
    out.duration_ms = am_ms;

    // The broadcast is out; now wait for the app to write its result to the
    // result socket (for play, the result only arrives after
    // prepare()+start()).
    std::string result;
    const int acc = accept_with_timeout(out_fd, accept_timeout_ms);
    if (acc >= 0) {
        out.result_received = read_to_eof(acc, &result, accept_timeout_ms);
        ::close(acc);
    }
    ::close(in_fd);
    ::close(out_fd);
    if (out.result_received) {
        out.result = result;
        out.duration_ms = audiorouter::get_time_ms() - t0;
    }
    return out;
#else
    (void)action;
    (void)file;
    (void)accept_timeout_ms;
    return out;
#endif
}

// Preferred command path with results: the listen-socket protocol first (it
// is the fastest and measures prepare()+start() precisely), then am
// broadcast with result sockets. On Android 14+ (API 34+) the app's listen
// socket freezes, so the am path is used directly — exactly like the
// official termux-api binary.
MediaCommandOutcome send_media_command(const std::string& action, const std::string& file,
                                       int accept_timeout_ms) {
    MediaCommandOutcome out;
#if defined(__ANDROID__)
    if (android_sdk_int() < 34) {
        const uint64_t t0 = audiorouter::get_time_ms();
        std::string result;
        bool complete = false;
        if (try_socket_command(action, file, &result, accept_timeout_ms, &complete) ==
            SocketCommandResult::Executed) {
            out.delivered = true;
            out.result_received = complete;
            out.socket_path = true;
            out.result = result;
            out.duration_ms = audiorouter::get_time_ms() - t0;
            return out;
        }
    }
    out = send_am_socket_broadcast(action, file, accept_timeout_ms);
#else
    (void)action;
    (void)file;
    (void)accept_timeout_ms;
#endif
    return out;
}

// Player-level command path: respects the blind fallback (result_channel ==
// false), where commands go out as plain am broadcasts with no result wait.
MediaCommandOutcome media_command(std::atomic<bool>* result_channel, const std::string& action,
                                  const std::string& file, int accept_timeout_ms) {
    MediaCommandOutcome out;
#if defined(__ANDROID__)
    if (!result_channel->load()) {
        const uint64_t t0 = audiorouter::get_time_ms();
        std::string txt;
        out.delivered = send_am_broadcast(action, file, &txt);
        out.duration_ms = audiorouter::get_time_ms() - t0;
        out.result = txt;
        return out;
    }
    out = send_media_command(action, file, accept_timeout_ms);
#else
    (void)result_channel;
    (void)action;
    (void)file;
    (void)accept_timeout_ms;
#endif
    return out;
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
    // The issue cadence must be slower than the command latency so the
    // single issuer thread keeps up (continuous playback); see the header.
    const double sustain_ms = latency_est_ms + kIssueMarginMs;
    if (sustain_ms > target_ms) target_ms = sustain_ms;
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
    segment_explicit_ = false;
    device_name_ = device_name.empty() ? "termux" : device_name;
    if (device_name_.rfind("termux:", 0) == 0) {
        const std::string rest = device_name_.substr(7);
        if (!rest.empty()) {
            segment_explicit_ = true;
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

    // Pure-root case (the client's privilege drop did not happen): label and
    // own the cache now, while we still can.
    if (AndroidHelpers::is_running_as_root()) {
        prepare_cache_as_root();
    }
    if (!resolve_cache_dir()) return false;
    cleanup_stale_segments();
    if (!ensure_pool()) return false;

    // Result sockets must carry the app's SELinux context when this client
    // runs in a root-shell domain (captured by prepare_cache_as_root()
    // before the privilege drop). Plain app-user runs have it naturally.
    g_sockcreate_context = load_app_context(cache_dir_);
    if (!g_sockcreate_context.empty()) {
        LOG_DEBUG("TermuxApiPlayer: result sockets will be labeled with the app's "
                  "SELinux context (" << g_sockcreate_context << ")");
    }

    // Warm the API app process (and start its socket listener) before the
    // probe: a cold com.termux.api takes hundreds of ms to come up. Runs
    // detached - its `am startservice` can take seconds and must not blow
    // the client's 3 s device-open budget. Skipped as root — the app's
    // broadcast permission only accepts the Termux uid, so a root client
    // will fail the probe and the open supervisor falls back anyway.
    if (geteuid() != 0) {
        std::thread([] {
            std::lock_guard<std::mutex> lock(AndroidHelpers::subprocess_mutex());
            (void)::system((find_am() +
                            " startservice -n com.termux.api/.KeepAliveService 2>/dev/null")
                               .c_str());
        }).detach();
    }

    // Preflight: the Termux:API app must answer a media_player command. The
    // probe also decides the transport: listen socket, am broadcast with
    // result sockets, or blind am (old app without a result channel).
    MediaCommandOutcome probe = media_command(&result_channel_, "info", "", kProbeTimeoutMs);
    if (!probe.delivered) {
        LOG_ERROR("TermuxApiPlayer: the Termux:API app did not answer a media_player "
                  "command (is com.termux.api installed?)");
        LOG_ERROR("TermuxApiPlayer: install Termux:API from F-Droid (the "
                  "termux-media-player API), then retry with -d termux");
        return false;
    }
    socket_path_ = probe.socket_path;
    result_channel_.store(probe.result_received);
    consecutive_no_result_ = 0;
    if (probe.result_received) {
        // The app answers with result text: every play's command duration
        // includes prepare()+start(), so the EMA converges quickly.
        latency_est_ms_.store(socket_path_ ? kInitialLatencyMs
                                           : std::clamp(static_cast<double>(probe.duration_ms) +
                                                            kBlindSeedBiasMs,
                                                        kMinLatencyMs, kMaxLatencyMs));
        LOG_INFO("TermuxApiPlayer: app result channel: "
                 << (socket_path_ ? "listen socket" : "am broadcast + result sockets")
                 << "; app status: " << trim(probe.result));
    } else {
        // The app received the broadcast but could not write its result back
        // to the client's result socket. This is the signature of the client
        // running in a root-shell SELinux domain the confined app may not
        // connect to (setuid does not change the domain). Blind mode:
        // delivery only, fixed latency estimate, no watchdog.
        latency_est_ms_.store(std::clamp(static_cast<double>(probe.duration_ms) +
                                             kBlindSeedBiasMs,
                                         kMinLatencyMs, kMaxLatencyMs));
        LOG_WARN("TermuxApiPlayer: the app received the probe but returned no result "
                 "over the result socket - usually the client runs in a root-shell "
                 "context the app cannot connect to (SELinux). Playing blind via am "
                 "broadcast, fixed ~" << static_cast<uint32_t>(latency_est_ms_.load())
                 << " ms command estimate. Diagnostics: su -c 'logcat -d -s "
                 "ResultReturner TermuxApiReceiver'; running without -b (no su) avoids "
                 "this entirely.");
    }

    issue_wall_ms_.store(0);
    issued_frames_.store(0);
    drops_ = 0;
    last_drop_warn_ms_ = 0;
    seg_fd_ = -1;
    seg_path_.clear();
    seg_frames_ = 0;
    seg_index_ = 0;
    seg_start_wall_ms_ = 0;
    {
        std::lock_guard<std::mutex> lock(handoff_mutex_);
        pending_.valid = false;
    }

    is_open_.store(true);
    stop_issuer_.store(false);
    issuer_thread_ = std::thread(&TermuxApiPlayer::issuer_loop, this);
    stop_watchdog_.store(false);
    if (result_channel_.load()) {
        watchdog_thread_ = std::thread(&TermuxApiPlayer::watchdog_loop, this);
    }
    LOG_INFO("TermuxApiPlayer: Termux:API media player ready (" << segment_ms_
             << " ms segments, "
             << (result_channel_.load() ? (socket_path_ ? "socket protocol"
                                                        : "am + result sockets")
                                        : "blind am broadcast")
             << ", cache " << cache_dir_ << ")");
    return true;
#else
    (void)config;
    (void)device_name;
    LOG_INFO("TermuxApiPlayer: Termux:API playback not available on this platform");
    return false;
#endif
}

void TermuxApiPlayer::prepare_cache_as_root() {
#if defined(__ANDROID__)
    if (!AndroidHelpers::is_running_as_root()) return;
    std::string termux_home;
    if (!AndroidHelpers::termux_user(nullptr, nullptr, &termux_home)) return;
    const std::string dir = termux_home + "/.audiorouter";
    (void)::mkdir(dir.c_str(), 0777);
    ::chmod(dir.c_str(), 0777);

    // Create the segment pool and label it with the app-data SELinux context
    // while we still have root: after the privilege drop new inodes cannot be
    // relabeled, and com.termux.api can only read app_data_file files.
    for (int i = 0; i < kPoolSize; ++i) {
        const std::string p = dir + "/seg_p" + std::to_string(i) + ".wav";
        const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT, 0666);
        if (fd >= 0) ::close(fd);
    }
    {
        std::lock_guard<std::mutex> lock(AndroidHelpers::subprocess_mutex());
        (void)::system(("restorecon -RF " + dir + " 2>/dev/null").c_str());
    }
    // Hand the directory and pool to the Termux app user so the dropped
    // client can open the files for writing.
    uid_t uid = 0;
    gid_t gid = 0;
    if (AndroidHelpers::termux_user(&uid, &gid, nullptr)) {
        ::chown(dir.c_str(), uid, gid);
        for (int i = 0; i < kPoolSize; ++i) {
            const std::string p = dir + "/seg_p" + std::to_string(i) + ".wav";
            ::chown(p.c_str(), uid, gid);
            ::chmod(p.c_str(), 0666);
        }
        // Capture the app's SELinux domain while we can still read it (the
        // dropped client labels its result sockets with it so the confined
        // app can connect to them - see create_listener_socket()).
        const std::string ctx = find_app_context(uid);
        if (!ctx.empty()) {
            const std::string p = dir + "/app_context";
            const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                (void)::write(fd, ctx.data(), ctx.size());
                ::close(fd);
            }
            ::chown(p.c_str(), uid, gid);
            ::chmod(p.c_str(), 0644);
        }
    }
#endif
}

bool TermuxApiPlayer::resolve_cache_dir() {
#if defined(__ANDROID__)
    std::string base;
    if (geteuid() != 0) {
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
        // Old naming (seg_<n>.wav) leftovers only — the recycled pool files
        // (seg_p<n>.wav) must stay, their labels survive the privilege drop.
        if (name.rfind("seg_", 0) != 0 || name.rfind("seg_p", 0) == 0) continue;
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".wav") != 0) continue;
        ::unlink((cache_dir_ + "/" + name).c_str());
    }
    ::closedir(dir);
}

bool TermuxApiPlayer::ensure_pool() {
#if defined(__ANDROID__)
    for (int i = 0; i < kPoolSize; ++i) {
        const std::string p = cache_dir_ + "/seg_p" + std::to_string(i) + ".wav";
        if (::access(p.c_str(), W_OK) == 0) continue;
        const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            LOG_ERROR("TermuxApiPlayer: cannot create pool file " << p << ": "
                      << std::strerror(errno));
            return false;
        }
        ::close(fd);
    }
    return true;
#else
    return false;
#endif
}

bool TermuxApiPlayer::open_next_segment_locked() {
#if defined(__ANDROID__)
    if (seg_fd_ >= 0) {
        ::close(seg_fd_);
        seg_fd_ = -1;
    }
    ++seg_index_;
    // Recycle pool inodes (O_TRUNC): the SELinux label of the file was fixed
    // when the pool was created, so the app keeps read access regardless of
    // the domain of this process.
    seg_path_ = cache_dir_ + "/seg_p" + std::to_string(seg_index_ % kPoolSize) + ".wav";
    seg_fd_ = ::open(seg_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (seg_fd_ < 0) {
        LOG_ERROR("TermuxApiPlayer: cannot open " << seg_path_ << ": " << std::strerror(errno));
        return false;
    }
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
        seg_path_.clear();
        return false;
    }
    seg_frames_ = 0;
    seg_start_wall_ms_ = get_time_ms();
    return true;
#else
    return false;
#endif
}

void TermuxApiPlayer::discard_segment_locked() {
    if (seg_fd_ >= 0) {
        ::close(seg_fd_);
        seg_fd_ = -1;
    }
    seg_path_.clear();
    seg_frames_ = 0;
    seg_start_wall_ms_ = get_time_ms();  // re-anchor at live audio
}

void TermuxApiPlayer::finalize_segment_locked() {
#if defined(__ANDROID__)
    if (seg_fd_ < 0) return;
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
    const uint64_t idx = seg_index_;
    ::close(seg_fd_);
    seg_fd_ = -1;

    // Hand off to the issuer thread (never inline: a slow play command must
    // not stall the jitter-buffer pops on this thread).
    bool replaced = false;
    {
        std::lock_guard<std::mutex> hlock(handoff_mutex_);
        if (pending_.valid) {
            // The issuer is still executing the previous play: the pending
            // segment was never heard; replace it with this fresher one.
            replaced = true;
        }
        pending_ = PendingSegment{path, frames, idx, true};
    }
    handoff_cv_.notify_one();
    if (replaced) {
        ++drops_;
        const uint64_t now_ms = get_time_ms();
        if (now_ms - last_drop_warn_ms_ >= 5000) {
            last_drop_warn_ms_ = now_ms;
            LOG_WARN("TermuxApiPlayer: media player commands are slower than the segment "
                     "length; dropping ready segments to stay at live audio (" << drops_
                     << " dropped so far; -d termux:<larger ms> avoids this)");
        }
    }
    open_next_segment_locked();
#endif
}

void TermuxApiPlayer::issuer_loop() {
#if defined(__ANDROID__)
    while (true) {
        PendingSegment job;
        {
            std::unique_lock<std::mutex> lock(handoff_mutex_);
            handoff_cv_.wait(lock, [this] { return stop_issuer_.load() || pending_.valid; });
            if (stop_issuer_.load()) break;  // shutdown: drop whatever is pending
            job = pending_;
            pending_.valid = false;
        }

        // This is the only place that talks to the Termux:API app; blocking
        // here is fine — the recording engine and the jitter buffer keep
        // running on their own threads.
        MediaCommandOutcome out = media_command(&result_channel_, "play", job.path,
                                                kPlayTimeoutMs);

        if (out.delivered) {
            issue_wall_ms_.store(get_time_ms());
            issued_frames_.store(job.frames);
            if (out.result_received) {
                // The app's own confirmation arrived (after prepare+start):
                // a real latency sample AND proof that playback started.
                consecutive_no_result_ = 0;
                const double sample = static_cast<double>(out.duration_ms);
                const double prev = latency_est_ms_.load();
                latency_est_ms_.store(std::clamp(prev * 0.65 + sample * 0.35, kMinLatencyMs,
                                                 kMaxLatencyMs));
                LOG_INFO("TermuxApiPlayer: media player: " << trim(out.result) << " (command "
                         << out.duration_ms << " ms)");
            } else {
                ++consecutive_no_result_;
                if (consecutive_no_result_ >= 2 && result_channel_.exchange(false)) {
                    LOG_WARN("TermuxApiPlayer: the app returned no result twice; switching "
                             "to blind am broadcast. The app received the broadcasts but "
                             "cannot reach the client's result socket - usually a root-shell "
                             "SELinux context; run without -b (no su), or check: su -c "
                             "'logcat -d -s ResultReturner TermuxApiReceiver'");
                } else if (result_channel_.load()) {
                    LOG_WARN("TermuxApiPlayer: play delivered but the app returned no "
                             "result (" << consecutive_no_result_ << "); check: su -c "
                             "'logcat -d -s ResultReturner TermuxApiReceiver'");
                } else {
                    LOG_DEBUG("TermuxApiPlayer: play delivered (blind mode)");
                }
            }
        } else {
            LOG_WARN("TermuxApiPlayer: play command failed"
                     << (out.result.empty() ? std::string() : ": " + trim(out.result)));
            // Keep the issued bookkeeping as-is: a previously playing
            // segment runs to its EOF, and the next ready segment restarts
            // playback at live audio.
        }
    }
#endif
}

void TermuxApiPlayer::watchdog_loop() {
#if defined(__ANDROID__)
    while (!stop_watchdog_.load()) {
        sleep_ms(kWatchdogPollMs);
        if (stop_watchdog_.load() || !is_open_.load()) break;
        if (!result_channel_.load()) continue;  // blind mode: no status channel
        if (issued_frames_.load() == 0) continue;

        MediaCommandOutcome info = media_command(&result_channel_, "info", "", kInfoTimeoutMs);
        if (!info.delivered || !info.result_received) continue;
        if (info.result.find("Paused") == std::string::npos) continue;
        LOG_WARN("TermuxApiPlayer: media player paused (audio focus?); resuming");
        MediaCommandOutcome resume =
            media_command(&result_channel_, "resume", "", kInfoTimeoutMs);
        if (!resume.delivered) {
            LOG_WARN("TermuxApiPlayer: resume command did not reach the app");
        }
    }
#endif
}

void TermuxApiPlayer::close() {
#if defined(__ANDROID__)
    stop_watchdog_.store(true);
    if (watchdog_thread_.joinable()) watchdog_thread_.join();
    stop_issuer_.store(true);
    handoff_cv_.notify_all();
    if (issuer_thread_.joinable()) issuer_thread_.join();

    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!is_open_.exchange(false)) return;

    // Politely stop whatever is playing (fire-and-forget am; the track would
    // also end by itself at the segment EOF).
    if (issued_frames_.load() > 0) {
        std::string ignored;
        (void)send_am_broadcast("stop", "", &ignored);
    }
    if (seg_fd_ >= 0) {
        ::close(seg_fd_);
        seg_fd_ = -1;
    }
    seg_path_.clear();
    seg_frames_ = 0;
    issued_frames_.store(0);
    issue_wall_ms_.store(0);
    {
        std::lock_guard<std::mutex> hlock(handoff_mutex_);
        pending_.valid = false;
    }
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
    return send_media_command("info", "", kProbeTimeoutMs).delivered;
#else
    return false;
#endif
}

size_t TermuxApiPlayer::write_frames(const void* pcm_data, size_t num_frames) {
    if (pcm_data == nullptr || num_frames == 0) return 0;
#if defined(__ANDROID__)
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!is_open_.load()) return 0;

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

    if (seg_fd_ < 0 && !open_next_segment_locked()) return 0;

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

    // Pace at real time: the segment file absorbs writes instantly, so
    // without pacing the playback thread would drain the jitter buffer in a
    // tight loop - PLC would stuff the segment with minutes of silence, the
    // wall-clock gate would never fire, and get_buffer_delay_frames() would
    // report an ever-growing backlog (the 'Audio:' status ran away to
    // hundreds of seconds on-device). Sleeping the chunk duration records at
    // exactly real-time rate, like DummyPlayer.
    const uint64_t duration_us = (static_cast<uint64_t>(num_frames) * 1000000ULL) / rate;
    if (duration_us > 0) sleep_us(static_cast<uint32_t>(duration_us));

    // Hand the segment to the issuer once it holds enough audio AND the wall
    // clock agrees (gapless chaining; see termux_api::segment_ready).
    termux_api::SegmentClock clock{seg_start_wall_ms_, seg_frames_, rate};
    if (termux_api::segment_ready(clock, get_time_ms(), segment_ms_, latency_est_ms_.load(),
                                  kIssueLeadMs)) {
        finalize_segment_locked();
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

    // Frames recorded into the open (not yet finalized) segment.
    const size_t unissued = seg_frames_;
    // A finalized segment still waiting for its play command.
    size_t pending_frames = 0;
    {
        std::lock_guard<std::mutex> hlock(handoff_mutex_);
        if (pending_.valid) pending_frames = pending_.frames;
    }

    // Remainder of the issued segment that has not been heard yet, estimated
    // from the wall clock (MediaPlayer plays at real time from issue+start).
    const double lat = latency_est_ms_.load();
    const size_t issued = issued_frames_.load();
    const uint64_t issue_wall = issue_wall_ms_.load();
    size_t issued_remaining = 0;
    if (issued > 0 && issue_wall > 0) {
        const uint64_t now_ms = get_time_ms();
        if (now_ms > issue_wall) {
            const double played_ms = static_cast<double>(now_ms - issue_wall) - lat;
            if (played_ms > 0) {
                const size_t played = static_cast<size_t>(played_ms * rate / 1000.0);
                issued_remaining = played < issued ? issued - played : 0;
            } else {
                issued_remaining = issued;
            }
        }
    }
    // MediaPlayer's own buffered lookahead (~ its startup latency).
    const size_t in_player = static_cast<size_t>(lat * rate / 1000.0);
    return unissued + pending_frames + issued_remaining + in_player;
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
    {
        std::lock_guard<std::mutex> hlock(handoff_mutex_);
        pending_.valid = false;
    }
}

std::string TermuxApiPlayer::get_device_name() const {
    return device_name_;
}

} // namespace audiorouter
