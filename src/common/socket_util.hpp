#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <memory>

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
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <netdb.h>
    #include <ifaddrs.h>
    #include <net/if.h>
    #include <sys/select.h>
    using socket_t = int;
    #define INVALID_SOCKET_HANDLE (-1)
    #define SOCKET_ERROR_VAL (-1)
#endif

namespace audiorouter {

struct NetworkInterfaceInfo {
    std::string name;
    std::string ip_address;
    bool is_loopback;
    bool is_up;
};

class SocketAddress {
public:
    SocketAddress();
    SocketAddress(const std::string& ip_str, uint16_t port);
    explicit SocketAddress(const struct sockaddr_in& addr);

    bool is_valid() const;
    std::string ip() const;
    uint16_t port() const;
    std::string to_string() const;

    const struct sockaddr_in& raw() const { return addr_; }
    struct sockaddr_in& raw() { return addr_; }
    const struct sockaddr* sockaddr_ptr() const {
        return reinterpret_cast<const struct sockaddr*>(&addr_);
    }
    struct sockaddr* sockaddr_ptr() {
        return reinterpret_cast<struct sockaddr*>(&addr_);
    }
    static constexpr size_t size() { return sizeof(struct sockaddr_in); }

    bool operator==(const SocketAddress& other) const;
    bool operator!=(const SocketAddress& other) const;

private:
    struct sockaddr_in addr_;
    bool valid_;
};

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    // Sockets are non-copyable but movable
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    bool open();
    void close();
    bool is_open() const;

    bool bind(uint16_t port, const std::string& ip = "0.0.0.0");
    int send_to(const void* data, size_t size, const SocketAddress& dest);
    int receive_from(void* buffer, size_t max_size, SocketAddress& out_source);

    bool set_non_blocking(bool enable);
    bool set_receive_timeout_ms(int ms);
    bool set_send_timeout_ms(int ms);
    bool set_broadcast(bool enable);
    bool set_buffer_sizes(int recv_size_bytes, int send_size_bytes);
    bool set_qos_priority(bool enable); // Sets DSCP / IP TOS for low-latency audio

    SocketAddress get_local_address() const;

    // Raw handle for poll()-style multiplexing (USB tunnel relay).
    socket_t native_handle() const { return handle_; }

    // Forces all traffic on this socket out the named interface (SO_BINDTODEVICE).
    // Linux/Android only (needs root / CAP_NET_RAW); on Windows this is a no-op
    // returning false. Use it to bypass an Android VPN tunnel (tun0) so LAN
    // audio traffic goes straight over Wi-Fi.
    bool bind_to_interface(const std::string& ifname);

    static void init_networking();
    static void cleanup_networking();
    static std::vector<NetworkInterfaceInfo> get_local_interfaces();
    static std::string get_last_error_string();

    // Best-effort pick of the physical (non-virtual, non-loopback) interface,
    // e.g. "wlan0" on a phone with an active VPN (tun0). Empty if none found.
    static std::string pick_physical_interface();

private:
    socket_t handle_;
    bool is_bound_;
};

// Minimal TCP socket for the Voice-over-USB tunnel (adb reverse tcp:).
// The relay in client.cpp/server.cpp multiplexes it with select(), so
// connect()/accept() leave the socket in NON-blocking mode; read/write
// helpers in usb_tunnel.hpp handle EAGAIN by waiting on select().
class TcpSocket {
public:
    TcpSocket();
    ~TcpSocket();

    // Sockets are non-copyable but movable
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    bool open();
    void close();
    bool is_open() const;

    // Blocking connect with a timeout (select-based). Leaves the socket
    // non-blocking so the caller can multiplex it with select().
    bool connect(const SocketAddress& addr, int timeout_ms);
    // Listen on ip:port (loopback in USB mode). Does not alter blocking mode.
    bool listen(uint16_t port, const std::string& ip, int backlog);
    // Accept one connection; select-based timeout (0 = timeout, -1 = error,
    // 1 = success). The accepted socket is left non-blocking.
    bool accept(TcpSocket& out_client, int timeout_ms);

    // Plain send/recv (non-blocking; EAGAIN/EWOULDBLOCK surfaces as -1).
    int send(const void* data, size_t size);
    int recv(void* buffer, size_t max_size);

    // 1 = ready, 0 = timeout, -1 = error.
    int wait_readable(int timeout_ms);
    int wait_writable(int timeout_ms);

    bool set_tcp_nodelay(bool enable);
    bool set_non_blocking(bool enable);

    SocketAddress get_local_address() const;
    socket_t native_handle() const { return handle_; }

private:
    socket_t handle_;
};

} // namespace audiorouter
