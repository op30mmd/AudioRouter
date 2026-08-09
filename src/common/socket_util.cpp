#include "socket_util.hpp"
#include "logger.hpp"

#include <cstring>
#include <sstream>
#include <mutex>
#include <atomic>
#include <array>

#if defined(_WIN32)
    #include <iphlpapi.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "iphlpapi.lib")
#endif

namespace audiorouter {

namespace {
    std::once_flag g_init_flag;
    std::atomic<bool> g_networking_initialized{false};
    std::mutex g_init_mutex; // fallback if once_flag not enough for cleanup
}

void UdpSocket::init_networking() {
    std::call_once(g_init_flag, []{
#if defined(_WIN32)
        WSADATA wsa_data{};
        int res = WSAStartup(MAKEWORD(2, 2), &wsa_data);
        if (res != 0) {
            LOG_ERROR("WSAStartup failed: " << res);
        } else {
            g_networking_initialized.store(true, std::memory_order_release);
        }
#else
        g_networking_initialized.store(true, std::memory_order_release);
#endif
    });
}

void UdpSocket::cleanup_networking() {
    if (!g_networking_initialized.load(std::memory_order_acquire)) return;
#if defined(_WIN32)
    WSACleanup();
#endif
    g_networking_initialized.store(false, std::memory_order_release);
}

std::string UdpSocket::get_last_error_string() {
#if defined(_WIN32)
    int err = WSAGetLastError();
    char* s = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&s, 0, nullptr);
    std::string result = s ? s : ("Winsock error: " + std::to_string(err));
    if (s) LocalFree(s);
    return result;
#else
    // strerror is not thread-safe; use strerror_r
    char buf[256]{};
#if defined(__GLIBC__) && (_GNU_SOURCE)
    char* msg = strerror_r(errno, buf, sizeof(buf));
    return msg ? std::string(msg) : std::string("errno ") + std::to_string(errno);
#else
    if (strerror_r(errno, buf, sizeof(buf)) == 0) return std::string(buf);
    return std::string("errno ") + std::to_string(errno);
#endif
#endif
}

// ── SocketAddress ──

SocketAddress::SocketAddress() noexcept : valid_(false) {
    std::memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
}

SocketAddress::SocketAddress(std::string_view ip_str, uint16_t port) : valid_(false) {
    std::memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(port);
    addr_.sin_addr.s_addr = INADDR_ANY; // default

    if (ip_str.empty() || ip_str == "0.0.0.0") {
        addr_.sin_addr.s_addr = INADDR_ANY;
        valid_ = true;
        return;
    }
    if (ip_str == "255.255.255.255") {
        addr_.sin_addr.s_addr = INADDR_BROADCAST;
        valid_ = true;
        return;
    }
    // Try numeric parse first — thread-safe
    std::string tmp(ip_str); // inet_pton needs NUL
    int res = inet_pton(AF_INET, tmp.c_str(), &addr_.sin_addr);
    if (res == 1) { valid_ = true; return; }
    if (res == 0) {
        // Try DNS resolution via getaddrinfo (thread-safe replacement for gethostbyname)
        struct addrinfo hints{}, *result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        // Limit to numeric? No — allow hostname. Use tmp.c_str() with port hint needed? Use ip_str.
        int gai = getaddrinfo(tmp.c_str(), nullptr, &hints, &result);
        if (gai == 0 && result) {
            // Take first AF_INET result
            for (auto* rp = result; rp; rp = rp->ai_next) {
                if (rp->ai_family == AF_INET && rp->ai_addrlen >= sizeof(sockaddr_in)) {
                    std::memcpy(&addr_, rp->ai_addr, sizeof(sockaddr_in));
                    // Preserve requested port (getaddrinfo with nullptr service leaves 0)
                    addr_.sin_port = htons(port);
                    valid_ = true;
                    break;
                }
            }
            freeaddrinfo(result);
            if (valid_) return;
        }
        // If hostname resolution fails, invalid
        valid_ = false;
        std::memset(&addr_, 0, sizeof(addr_));
        addr_.sin_family = AF_INET;
    } else {
        // inet_pton error (-1) → invalid
        valid_ = false;
    }
}

SocketAddress::SocketAddress(const struct sockaddr_in& addr) noexcept : addr_(addr), valid_(true) {}

std::expected<SocketAddress, std::string> SocketAddress::create(std::string_view ip_str, uint16_t port) noexcept {
    SocketAddress a(ip_str, port);
    if (!a.is_valid()) return std::unexpected<std::string>(std::string("invalid address: ") + std::string(ip_str) + ":" + std::to_string(port));
    return a;
}

bool SocketAddress::is_valid() const noexcept { return valid_; }

std::string SocketAddress::ip() const {
    if (!valid_) return "0.0.0.0";
    char buffer[INET_ADDRSTRLEN]{};
    const char* res = inet_ntop(AF_INET, &addr_.sin_addr, buffer, sizeof(buffer));
    return res ? std::string(buffer) : std::string("0.0.0.0");
}

uint16_t SocketAddress::port() const noexcept {
    if (!valid_) return 0;
    return ntohs(addr_.sin_port);
}

std::string SocketAddress::to_string() const {
    return ip() + ":" + std::to_string(port());
}

bool SocketAddress::operator==(const SocketAddress& other) const noexcept {
    if (valid_ != other.valid_) return false;
    if (!valid_ && !other.valid_) return true;
    return (addr_.sin_family == other.addr_.sin_family) &&
           (addr_.sin_port == other.addr_.sin_port) &&
           (addr_.sin_addr.s_addr == other.addr_.sin_addr.s_addr);
}
bool SocketAddress::operator!=(const SocketAddress& other) const noexcept { return !(*this == other); }

// ── UdpSocket ──

UdpSocket::UdpSocket() : handle_(INVALID_SOCKET_HANDLE), is_bound_(false) { init_networking(); }
UdpSocket::~UdpSocket() { close(); }

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : handle_(other.handle_), is_bound_(other.is_bound_) {
    other.handle_ = INVALID_SOCKET_HANDLE;
    other.is_bound_ = false;
}
UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        is_bound_ = other.is_bound_;
        other.handle_ = INVALID_SOCKET_HANDLE;
        other.is_bound_ = false;
    }
    return *this;
}

bool UdpSocket::open() {
    if (is_open()) return true;
    handle_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle_ == INVALID_SOCKET_HANDLE) {
        LOG_ERROR("Failed to create UDP socket: " << get_last_error_string());
        return false;
    }
    // Harden: default buffers 512KB, and set close-on-exec
#if !defined(_WIN32)
    int flags = fcntl(handle_, F_GETFD);
    if (flags != -1) fcntl(handle_, F_SETFD, flags | FD_CLOEXEC);
#endif
    set_buffer_sizes(512 * 1024, 512 * 1024);
    return true;
}

void UdpSocket::close() noexcept {
    if (handle_ != INVALID_SOCKET_HANDLE) {
#if defined(_WIN32)
        closesocket(handle_);
#else
        ::close(handle_);
#endif
        handle_ = INVALID_SOCKET_HANDLE;
        is_bound_ = false;
    }
}

bool UdpSocket::is_open() const noexcept { return handle_ != INVALID_SOCKET_HANDLE; }

bool UdpSocket::bind(uint16_t port, std::string_view ip) {
    if (!is_open() && !open()) return false;
    int reuse = 1;
#if defined(_WIN32)
    setsockopt(handle_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
    setsockopt(handle_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    // Validate port range (port is uint16_t so always valid, but check)
    SocketAddress bind_addr(ip, port);
    if (!bind_addr.is_valid()) {
        LOG_ERROR("Invalid bind address: " << std::string(ip) << ":" << port);
        return false;
    }
    int res = ::bind(handle_, bind_addr.sockaddr_ptr(), static_cast<socklen_t>(SocketAddress::size()));
    if (res == SOCKET_ERROR_VAL) {
        LOG_ERROR("Socket bind failed on " << std::string(ip) << ":" << port << " - " << get_last_error_string());
        return false;
    }
    is_bound_ = true;
    LOG_INFO("UDP socket bound to " << bind_addr.to_string());
    return true;
}

int UdpSocket::send_to(const void* data, size_t size, const SocketAddress& dest) {
    if (!data || size == 0) return -1;
    if (size > static_cast<size_t>(std::numeric_limits<int>::max())) return -1; // prevent truncation
    if (!dest.is_valid()) return -1;
    if (!is_open() && !open()) return -1;
#if defined(_WIN32)
    int bytes = ::sendto(handle_, static_cast<const char*>(data), static_cast<int>(size),
                         0, dest.sockaddr_ptr(), static_cast<int>(SocketAddress::size()));
#else
    ssize_t bytes = ::sendto(handle_, data, size, 0, dest.sockaddr_ptr(), static_cast<socklen_t>(SocketAddress::size()));
#endif
    return static_cast<int>(bytes);
}

int UdpSocket::receive_from(void* buffer, size_t max_size, SocketAddress& out_source) {
    if (!buffer || max_size == 0) return -1;
    if (max_size > static_cast<size_t>(std::numeric_limits<int>::max())) return -1;
    if (!is_open()) return -1;
    struct sockaddr_in src_addr{};
    socklen_t addr_len = sizeof(src_addr);
#if defined(_WIN32)
    int bytes = ::recvfrom(handle_, static_cast<char*>(buffer), static_cast<int>(max_size),
                           0, reinterpret_cast<struct sockaddr*>(&src_addr), &addr_len);
#else
    ssize_t bytes = ::recvfrom(handle_, buffer, max_size, 0, reinterpret_cast<struct sockaddr*>(&src_addr), &addr_len);
#endif
    if (bytes >= 0) out_source = SocketAddress(src_addr);
    return static_cast<int>(bytes);
}

std::expected<size_t, std::string> UdpSocket::send_to(std::span<const std::byte> data, const SocketAddress& dest) noexcept {
    if (data.empty()) return std::unexpected<std::string>(std::string("send_to: empty span"));
    if (!dest.is_valid()) return std::unexpected<std::string>(std::string("send_to: invalid destination"));
    if (!is_open()) {
        if (!open()) return std::unexpected<std::string>(std::string("send_to: socket open failed: ") + get_last_error_string());
    }
    if (data.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        return std::unexpected<std::string>(std::string("send_to: data too large"));
#if defined(_WIN32)
    int bytes = ::sendto(handle_, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()),
                         0, dest.sockaddr_ptr(), static_cast<int>(SocketAddress::size()));
    if (bytes == SOCKET_ERROR) return std::unexpected<std::string>(get_last_error_string());
    return static_cast<size_t>(bytes);
#else
    ssize_t bytes = ::sendto(handle_, data.data(), data.size(), 0, dest.sockaddr_ptr(), static_cast<socklen_t>(SocketAddress::size()));
    if (bytes < 0) return std::unexpected<std::string>(get_last_error_string());
    return static_cast<size_t>(bytes);
#endif
}

std::expected<size_t, std::string> UdpSocket::receive_from(std::span<std::byte> buffer, SocketAddress& out_source) noexcept {
    if (buffer.empty()) return std::unexpected<std::string>(std::string("receive_from: empty buffer"));
    if (!is_open()) return std::unexpected<std::string>(std::string("receive_from: socket not open"));
    struct sockaddr_in src_addr{};
    socklen_t addr_len = sizeof(src_addr);
#if defined(_WIN32)
    int bytes = ::recvfrom(handle_, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()),
                           0, reinterpret_cast<struct sockaddr*>(&src_addr), &addr_len);
    if (bytes == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) return std::unexpected<std::string>(std::string("timeout"));
        return std::unexpected<std::string>(get_last_error_string());
    }
    out_source = SocketAddress(src_addr);
    return static_cast<size_t>(bytes);
#else
    ssize_t bytes = ::recvfrom(handle_, buffer.data(), buffer.size(), 0, reinterpret_cast<struct sockaddr*>(&src_addr), &addr_len);
    if (bytes < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return std::unexpected<std::string>(std::string("timeout"));
        return std::unexpected<std::string>(get_last_error_string());
    }
    out_source = SocketAddress(src_addr);
    return static_cast<size_t>(bytes);
#endif
}

bool UdpSocket::set_non_blocking(bool enable) noexcept {
    if (!is_open()) return false;
#if defined(_WIN32)
    u_long mode = enable ? 1 : 0;
    return ioctlsocket(handle_, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(handle_, F_GETFL, 0);
    if (flags < 0) return false;
    flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(handle_, F_SETFL, flags) == 0;
#endif
}

bool UdpSocket::set_receive_timeout_ms(int ms) noexcept {
    if (!is_open()) return false;
    if (ms < 0) return false;
#if defined(_WIN32)
    DWORD timeout = static_cast<DWORD>(ms);
    return setsockopt(handle_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0;
#else
    struct timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return setsockopt(handle_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

bool UdpSocket::set_send_timeout_ms(int ms) noexcept {
    if (!is_open()) return false;
    if (ms < 0) return false;
#if defined(_WIN32)
    DWORD timeout = static_cast<DWORD>(ms);
    return setsockopt(handle_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0;
#else
    struct timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return setsockopt(handle_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

bool UdpSocket::set_broadcast(bool enable) noexcept {
    if (!is_open()) return false;
    int opt = enable ? 1 : 0;
#if defined(_WIN32)
    return setsockopt(handle_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0;
#else
    return setsockopt(handle_, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt)) == 0;
#endif
}

bool UdpSocket::set_buffer_sizes(int recv_size_bytes, int send_size_bytes) noexcept {
    if (!is_open()) return false;
    bool ok = true;
    if (recv_size_bytes > 0) {
        if (recv_size_bytes > 8 * 1024 * 1024) return false; // sanity
#if defined(_WIN32)
        if (setsockopt(handle_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&recv_size_bytes), sizeof(recv_size_bytes)) != 0) ok = false;
#else
        if (setsockopt(handle_, SOL_SOCKET, SO_RCVBUF, &recv_size_bytes, sizeof(recv_size_bytes)) != 0) ok = false;
#endif
    }
    if (send_size_bytes > 0) {
        if (send_size_bytes > 8 * 1024 * 1024) return false;
#if defined(_WIN32)
        if (setsockopt(handle_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&send_size_bytes), sizeof(send_size_bytes)) != 0) ok = false;
#else
        if (setsockopt(handle_, SOL_SOCKET, SO_SNDBUF, &send_size_bytes, sizeof(send_size_bytes)) != 0) ok = false;
#endif
    }
    return ok;
}

bool UdpSocket::set_qos_priority(bool enable) noexcept {
    if (!is_open()) return false;
    int tos = enable ? 0xB8 : 0x00;
#if defined(_WIN32)
    return setsockopt(handle_, IPPROTO_IP, IP_TOS, reinterpret_cast<const char*>(&tos), sizeof(tos)) == 0;
#else
    return setsockopt(handle_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) == 0;
#endif
}

SocketAddress UdpSocket::get_local_address() const noexcept {
    if (!is_open()) return SocketAddress();
    struct sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(handle_, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0) return SocketAddress(addr);
    return SocketAddress();
}

std::vector<NetworkInterfaceInfo> UdpSocket::get_local_interfaces() {
    std::vector<NetworkInterfaceInfo> list;
    init_networking();
#if defined(_WIN32)
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG out_buf_len = 15000;
    PIP_ADAPTER_ADDRESSES p_addresses = static_cast<PIP_ADAPTER_ADDRESSES>(malloc(out_buf_len));
    if (!p_addresses) return list;
    auto free_guard = std::unique_ptr<std::byte[], decltype(&free)>(reinterpret_cast<std::byte*>(p_addresses), &free);
    DWORD dw = GetAdaptersAddresses(AF_INET, flags, nullptr, p_addresses, &out_buf_len);
    if (dw == ERROR_BUFFER_OVERFLOW) {
        free_guard.reset();
        p_addresses = static_cast<PIP_ADAPTER_ADDRESSES>(malloc(out_buf_len));
        if (!p_addresses) return list;
        free_guard.reset(reinterpret_cast<std::byte*>(p_addresses));
        dw = GetAdaptersAddresses(AF_INET, flags, nullptr, p_addresses, &out_buf_len);
    }
    if (dw == NO_ERROR) {
        for (auto* cur = p_addresses; cur; cur = cur->Next) {
            if (cur->OperStatus != IfOperStatusUp) continue;
            for (auto* uni = cur->FirstUnicastAddress; uni; uni = uni->Next) {
                if (uni->Address.lpSockaddr->sa_family != AF_INET) continue;
                auto* sa_in = reinterpret_cast<sockaddr_in*>(uni->Address.lpSockaddr);
                char ip_buf[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &sa_in->sin_addr, ip_buf, INET_ADDRSTRLEN);
                NetworkInterfaceInfo info;
                char name_buf[256]{};
                WideCharToMultiByte(CP_UTF8, 0, cur->FriendlyName, -1, name_buf, sizeof(name_buf), nullptr, nullptr);
                info.name = name_buf;
                info.ip_address = ip_buf;
                info.is_loopback = (cur->IfType == IF_TYPE_SOFTWARE_LOOPBACK);
                info.is_up = true;
                list.push_back(std::move(info));
            }
        }
    }
#else
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) return list;
    std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> guard(ifaddr, &freeifaddrs);
    for (auto* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        char ip_buf[INET_ADDRSTRLEN]{};
        auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        if (!inet_ntop(AF_INET, &sa->sin_addr, ip_buf, INET_ADDRSTRLEN)) continue;
        NetworkInterfaceInfo info;
        info.name = ifa->ifa_name ? ifa->ifa_name : "unknown";
        info.ip_address = ip_buf;
        info.is_loopback = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
        info.is_up = (ifa->ifa_flags & IFF_UP) != 0;
        list.push_back(std::move(info));
    }
#endif
    return list;
}

} // namespace audiorouter
