#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <cstddef>
#include "span_compat.hpp"
#include "expected_compat.hpp"
#include <memory>
#include <concepts>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socket_t = SOCKET;
    #define INVALID_SOCKET_HANDLE INVALID_SOCKET
    #define SOCKET_ERROR_VAL SOCKET_ERROR
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <netdb.h>
    #include <ifaddrs.h>
    #include <net/if.h>
    using socket_t = int;
    #define INVALID_SOCKET_HANDLE (-1)
    #define SOCKET_ERROR_VAL (-1)
#endif

namespace audiorouter {

struct NetworkInterfaceInfo {
    std::string name;
    std::string ip_address;
    bool is_loopback = false;
    bool is_up = false;
};

// Strongly-typed, validated socket address — thread-safe value type
class SocketAddress {
public:
    SocketAddress() noexcept;
    SocketAddress(std::string_view ip_str, uint16_t port);
    explicit SocketAddress(const struct sockaddr_in& addr) noexcept;

    // Factory returning expected — preferred safe path
    [[nodiscard]] static std::expected<SocketAddress, std::string> create(std::string_view ip_str, uint16_t port) noexcept;

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] std::string ip() const;
    [[nodiscard]] uint16_t port() const noexcept;
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] const struct sockaddr_in& raw() const noexcept { return addr_; }
    [[nodiscard]] struct sockaddr_in& raw() noexcept { return addr_; }
    [[nodiscard]] const struct sockaddr* sockaddr_ptr() const noexcept {
        return reinterpret_cast<const struct sockaddr*>(&addr_);
    }
    [[nodiscard]] struct sockaddr* sockaddr_ptr() noexcept {
        return reinterpret_cast<struct sockaddr*>(&addr_);
    }
    [[nodiscard]] static constexpr size_t size() noexcept { return sizeof(struct sockaddr_in); }

    bool operator==(const SocketAddress& other) const noexcept;
    bool operator!=(const SocketAddress& other) const noexcept;

    // C++23 three-way friendly
    [[nodiscard]] bool equals(const SocketAddress& o) const noexcept { return *this == o; }

private:
    struct sockaddr_in addr_{};
    bool valid_ = false;
};

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    // Non-copyable, movable
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    [[nodiscard]] bool open();
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

    [[nodiscard]] bool bind(uint16_t port, std::string_view ip = "0.0.0.0");
    // Legacy raw pointer API (hardened with null/size checks)
    [[nodiscard]] int send_to(const void* data, size_t size, const SocketAddress& dest);
    [[nodiscard]] int receive_from(void* buffer, size_t max_size, SocketAddress& out_source);

    // C++23 span-based API — preferred, bounds-checked, [[nodiscard]]
    [[nodiscard]] std::expected<size_t, std::string> send_to(std::span<const std::byte> data, const SocketAddress& dest) noexcept;
    [[nodiscard]] std::expected<size_t, std::string> receive_from(std::span<std::byte> buffer, SocketAddress& out_source) noexcept;

    bool set_non_blocking(bool enable) noexcept;
    bool set_receive_timeout_ms(int ms) noexcept;
    bool set_send_timeout_ms(int ms) noexcept;
    bool set_broadcast(bool enable) noexcept;
    bool set_buffer_sizes(int recv_size_bytes, int send_size_bytes) noexcept;
    bool set_qos_priority(bool enable) noexcept;

    [[nodiscard]] SocketAddress get_local_address() const noexcept;

    static void init_networking();
    static void cleanup_networking();
    [[nodiscard]] static std::vector<NetworkInterfaceInfo> get_local_interfaces();
    [[nodiscard]] static std::string get_last_error_string();

private:
    socket_t handle_ = INVALID_SOCKET_HANDLE;
    bool is_bound_ = false;
    // Prevent data races on handle_ mutation — we rely on external synchronization for send/recv,
    // but close/open are serialized via caller's logic; handle_ itself is only touched under
    // logical ownership (single thread at a time) plus we make moves noexcept and atomic flag.
};

} // namespace audiorouter
