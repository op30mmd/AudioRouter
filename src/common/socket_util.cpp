#include "socket_util.hpp"
#include "logger.hpp"

#include <cstring>
#include <sstream>

#if defined(_WIN32)
    #include <iphlpapi.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "iphlpapi.lib")
#endif

namespace audiorouter {

namespace {
    static bool g_networking_initialized = false;
}

void UdpSocket::init_networking() {
    if (g_networking_initialized) return;
#if defined(_WIN32)
    WSADATA wsa_data;
    int res = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (res != 0) {
        LOG_ERROR("WSAStartup failed with error: " << res);
    } else {
        g_networking_initialized = true;
    }
#else
    g_networking_initialized = true;
#endif
}

void UdpSocket::cleanup_networking() {
    if (!g_networking_initialized) return;
#if defined(_WIN32)
    WSACleanup();
#endif
    g_networking_initialized = false;
}

std::string UdpSocket::get_last_error_string() {
#if defined(_WIN32)
    int err = WSAGetLastError();
    char* s = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&s, 0, NULL);
    std::string result = s ? s : ("Winsock error code: " + std::to_string(err));
    LocalFree(s);
    return result;
#else
    return strerror(errno);
#endif
}

// SocketAddress Implementation
SocketAddress::SocketAddress() : valid_(false) {
    std::memset(&addr_, 0, sizeof(addr_));
}

SocketAddress::SocketAddress(const std::string& ip_str, uint16_t port) : valid_(false) {
    std::memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(port);

    if (ip_str.empty() || ip_str == "0.0.0.0") {
        addr_.sin_addr.s_addr = INADDR_ANY;
        valid_ = true;
    } else if (ip_str == "255.255.255.255") {
        addr_.sin_addr.s_addr = INADDR_BROADCAST;
        valid_ = true;
    } else {
        int res = inet_pton(AF_INET, ip_str.c_str(), &addr_.sin_addr);
        if (res == 1) {
            valid_ = true;
        } else {
            // Try hostname resolution
            struct hostent* he = gethostbyname(ip_str.c_str());
            if (he && he->h_addr_list && he->h_addr_list[0]) {
                std::memcpy(&addr_.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
                valid_ = true;
            }
        }
    }
}

SocketAddress::SocketAddress(const struct sockaddr_in& addr) : addr_(addr), valid_(true) {}

bool SocketAddress::is_valid() const {
    return valid_;
}

std::string SocketAddress::ip() const {
    if (!valid_) return "0.0.0.0";
    char buffer[INET_ADDRSTRLEN];
    const char* res = inet_ntop(AF_INET, &addr_.sin_addr, buffer, sizeof(buffer));
    return res ? std::string(buffer) : "0.0.0.0";
}

uint16_t SocketAddress::port() const {
    if (!valid_) return 0;
    return ntohs(addr_.sin_port);
}

std::string SocketAddress::to_string() const {
    return ip() + ":" + std::to_string(port());
}

bool SocketAddress::operator==(const SocketAddress& other) const {
    return (addr_.sin_family == other.addr_.sin_family) &&
           (addr_.sin_port == other.addr_.sin_port) &&
           (addr_.sin_addr.s_addr == other.addr_.sin_addr.s_addr);
}

bool SocketAddress::operator!=(const SocketAddress& other) const {
    return !(*this == other);
}

// UdpSocket Implementation
UdpSocket::UdpSocket() : handle_(INVALID_SOCKET_HANDLE), is_bound_(false) {
    init_networking();
}

UdpSocket::~UdpSocket() {
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept 
    : handle_(other.handle_), is_bound_(other.is_bound_) {
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

    // Default socket buffers to at least 512KB for smooth audio streaming over Wi-Fi
    set_buffer_sizes(512 * 1024, 512 * 1024);

    return true;
}

void UdpSocket::close() {
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

bool UdpSocket::is_open() const {
    return handle_ != INVALID_SOCKET_HANDLE;
}

bool UdpSocket::bind(uint16_t port, const std::string& ip) {
    if (!is_open()) {
        if (!open()) return false;
    }

    // Enable SO_REUSEADDR
    int reuse = 1;
#if defined(_WIN32)
    setsockopt(handle_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#else
    setsockopt(handle_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    SocketAddress bind_addr(ip, port);
    if (!bind_addr.is_valid()) {
        LOG_ERROR("Invalid bind address: " << ip << ":" << port);
        return false;
    }

    int res = ::bind(handle_, bind_addr.sockaddr_ptr(), static_cast<socklen_t>(SocketAddress::size()));
    if (res == SOCKET_ERROR_VAL) {
        LOG_ERROR("Socket bind failed on " << ip << ":" << port << " - " << get_last_error_string());
        return false;
    }

    is_bound_ = true;
    LOG_INFO("UDP socket bound to " << bind_addr.to_string());
    return true;
}

int UdpSocket::send_to(const void* data, size_t size, const SocketAddress& dest) {
    if (!is_open()) {
        if (!open()) return -1;
    }
    if (!dest.is_valid()) return -1;

#if defined(_WIN32)
    int bytes = ::sendto(handle_, static_cast<const char*>(data), static_cast<int>(size),
                         0, dest.sockaddr_ptr(), static_cast<int>(SocketAddress::size()));
#else
    ssize_t bytes = ::sendto(handle_, data, size,
                             0, dest.sockaddr_ptr(), static_cast<socklen_t>(SocketAddress::size()));
#endif
    return static_cast<int>(bytes);
}

int UdpSocket::receive_from(void* buffer, size_t max_size, SocketAddress& out_source) {
    if (!is_open()) return -1;

    struct sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);
    std::memset(&src_addr, 0, sizeof(src_addr));

#if defined(_WIN32)
    int bytes = ::recvfrom(handle_, static_cast<char*>(buffer), static_cast<int>(max_size),
                           0, reinterpret_cast<struct sockaddr*>(&src_addr), &addr_len);
#else
    ssize_t bytes = ::recvfrom(handle_, buffer, max_size,
                               0, reinterpret_cast<struct sockaddr*>(&src_addr), &addr_len);
#endif

    if (bytes >= 0) {
        out_source = SocketAddress(src_addr);
    }
    return static_cast<int>(bytes);
}

bool UdpSocket::set_non_blocking(bool enable) {
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

bool UdpSocket::set_receive_timeout_ms(int ms) {
    if (!is_open()) return false;
#if defined(_WIN32)
    DWORD timeout = static_cast<DWORD>(ms);
    return setsockopt(handle_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == 0;
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return setsockopt(handle_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

bool UdpSocket::set_send_timeout_ms(int ms) {
    if (!is_open()) return false;
#if defined(_WIN32)
    DWORD timeout = static_cast<DWORD>(ms);
    return setsockopt(handle_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout)) == 0;
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return setsockopt(handle_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

bool UdpSocket::set_broadcast(bool enable) {
    if (!is_open()) return false;
    int opt = enable ? 1 : 0;
#if defined(_WIN32)
    return setsockopt(handle_, SOL_SOCKET, SO_BROADCAST, (const char*)&opt, sizeof(opt)) == 0;
#else
    return setsockopt(handle_, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt)) == 0;
#endif
}

bool UdpSocket::set_buffer_sizes(int recv_size_bytes, int send_size_bytes) {
    if (!is_open()) return false;
    bool ok = true;
    if (recv_size_bytes > 0) {
#if defined(_WIN32)
        if (setsockopt(handle_, SOL_SOCKET, SO_RCVBUF, (const char*)&recv_size_bytes, sizeof(recv_size_bytes)) != 0) {
            ok = false;
        }
#else
        if (setsockopt(handle_, SOL_SOCKET, SO_RCVBUF, &recv_size_bytes, sizeof(recv_size_bytes)) != 0) {
            ok = false;
        }
#endif
    }
    if (send_size_bytes > 0) {
#if defined(_WIN32)
        if (setsockopt(handle_, SOL_SOCKET, SO_SNDBUF, (const char*)&send_size_bytes, sizeof(send_size_bytes)) != 0) {
            ok = false;
        }
#else
        if (setsockopt(handle_, SOL_SOCKET, SO_SNDBUF, &send_size_bytes, sizeof(send_size_bytes)) != 0) {
            ok = false;
        }
#endif
    }
    return ok;
}

bool UdpSocket::set_qos_priority(bool enable) {
    if (!is_open()) return false;
    // Set IP TOS (Type of Service) / DSCP to Expedited Forwarding (0xB8) or CS6 (0xC0) for real-time audio
    int tos = enable ? 0xB8 : 0x00;
#if defined(_WIN32)
    return setsockopt(handle_, IPPROTO_IP, IP_TOS, (const char*)&tos, sizeof(tos)) == 0;
#else
    return setsockopt(handle_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) == 0;
#endif
}

SocketAddress UdpSocket::get_local_address() const {
    if (!is_open()) return SocketAddress();
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getsockname(handle_, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0) {
        return SocketAddress(addr);
    }
    return SocketAddress();
}

std::vector<NetworkInterfaceInfo> UdpSocket::get_local_interfaces() {
    std::vector<NetworkInterfaceInfo> list;
    init_networking();

#if defined(_WIN32)
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG out_buf_len = 15000;
    PIP_ADAPTER_ADDRESSES p_addresses = (IP_ADAPTER_ADDRESSES*)malloc(out_buf_len);

    if (p_addresses == nullptr) return list;

    DWORD dw_ret_val = GetAdaptersAddresses(AF_INET, flags, NULL, p_addresses, &out_buf_len);
    if (dw_ret_val == ERROR_BUFFER_OVERFLOW) {
        free(p_addresses);
        p_addresses = (IP_ADAPTER_ADDRESSES*)malloc(out_buf_len);
        if (p_addresses == nullptr) return list;
        dw_ret_val = GetAdaptersAddresses(AF_INET, flags, NULL, p_addresses, &out_buf_len);
    }

    if (dw_ret_val == NO_ERROR) {
        PIP_ADAPTER_ADDRESSES p_curr = p_addresses;
        while (p_curr) {
            if (p_curr->OperStatus == IfOperStatusUp) {
                PIP_ADAPTER_UNICAST_ADDRESS p_unicast = p_curr->FirstUnicastAddress;
                while (p_unicast) {
                    if (p_unicast->Address.lpSockaddr->sa_family == AF_INET) {
                        sockaddr_in* sa_in = (sockaddr_in*)p_unicast->Address.lpSockaddr;
                        char ip_buf[INET_ADDRSTRLEN] = {0};
                        inet_ntop(AF_INET, &(sa_in->sin_addr), ip_buf, INET_ADDRSTRLEN);

                        NetworkInterfaceInfo info;
                        char name_buf[256] = {0};
                        WideCharToMultiByte(CP_UTF8, 0, p_curr->FriendlyName, -1, name_buf, sizeof(name_buf), NULL, NULL);
                        info.name = name_buf;
                        info.ip_address = ip_buf;
                        info.is_loopback = (p_curr->IfType == IF_TYPE_SOFTWARE_LOOPBACK);
                        info.is_up = true;
                        list.push_back(info);
                    }
                    p_unicast = p_unicast->Next;
                }
            }
            p_curr = p_curr->Next;
        }
    }
    if (p_addresses) free(p_addresses);
#else
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        return list;
    }

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            char ip_buf[INET_ADDRSTRLEN] = {0};
            struct sockaddr_in* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
            inet_ntop(AF_INET, &(sa->sin_addr), ip_buf, INET_ADDRSTRLEN);

            NetworkInterfaceInfo info;
            info.name = ifa->ifa_name ? ifa->ifa_name : "unknown";
            info.ip_address = ip_buf;
            info.is_loopback = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
            info.is_up = (ifa->ifa_flags & IFF_UP) != 0;
            list.push_back(info);
        }
    }
    freeifaddrs(ifaddr);
#endif

    return list;
}

} // namespace audiorouter
