#pragma once

// Voice-over-USB tunnel framing (shared by the client and server relays).
//
// adb cannot forward UDP ("cannot bind listener: unknown socket
// specification:udp:<port>"), so the USB tunnel runs over
//   adb reverse tcp:<port> tcp:<port>
// and each UDP datagram is carried as a length-prefixed frame:
//
//   uint32 little-endian payload length | payload
//
// The client-side relay (phone) connects TCP to 127.0.0.1:<port> (adbd's
// reverse listener); the server-side relay (PC) listens on TCP 127.0.0.1:<port>
// where the adb server connects. Both relays multiplex the TCP socket and a
// loopback UDP socket with select() and translate datagrams <-> frames, so the
// protocol engines above them keep speaking plain UDP.

#include "socket_util.hpp"

#include <vector>
#include <cstdint>

namespace audiorouter {
namespace tunnel {

constexpr size_t kFrameHeaderSize = 4;      // uint32 LE payload length
constexpr size_t kMaxFramePayload  = 65536; // safety cap (audio frames are ~1 KB)

inline bool socket_would_block() {
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
#endif
}

// Polls up to two sockets at once. Returns a bitmask of ready sockets
// (1 = tcp, 2 = udp), 0 on timeout. One or both may be null.
inline int select2(TcpSocket* tcp, UdpSocket* udp, int timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    socket_t maxfd = 0;

    auto add_fd = [&maxfd, &rfds](socket_t fd) {
        if (fd != INVALID_SOCKET_HANDLE) {
            FD_SET(fd, &rfds);
            if (fd > maxfd) maxfd = fd;
        }
    };
    if (tcp && tcp->is_open()) add_fd(tcp->native_handle());
    if (udp && udp->is_open()) add_fd(udp->native_handle());

    if (maxfd == 0) return 0;

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int r = ::select(static_cast<int>(maxfd + 1), &rfds, nullptr, nullptr, &tv);
    if (r <= 0) return 0;

    int mask = 0;
    if (tcp && tcp->is_open() && FD_ISSET(tcp->native_handle(), &rfds)) mask |= 1;
    if (udp && udp->is_open() && FD_ISSET(udp->native_handle(), &rfds)) mask |= 2;
    return mask;
}

// Per-thread frame reassembly buffer. Shared through this accessor so that
// reset_frame_buffer() and read_frame() always touch the same buffer (a
// thread_local declared inside each function would be a DIFFERENT variable).
inline std::vector<uint8_t>& frame_rx_buffer() {
    thread_local std::vector<uint8_t> buf;
    return buf;
}

// Clears the persistent reassembly buffer read_frame() keeps. Must be called
// once after (re)establishing a TCP connection so leftover partial frames from
// a previous session can never be misread as the new session's data.
inline void reset_frame_buffer() {
    frame_rx_buffer().clear();
}

enum class RecvResult { Frame, Closed, Timeout, Error };

// Reads one length-prefixed frame from a non-blocking TCP socket. The socket
// should be select()-reported readable before the call; partial reads are
// buffered across calls. Returns Frame (with the payload in out), Closed on
// orderly EOF, Timeout when no further data arrived within the internal
// 20 ms wait, Error otherwise.
inline RecvResult read_frame(TcpSocket& sock, std::vector<uint8_t>& out) {
    std::vector<uint8_t>& s_rx_buf = frame_rx_buffer();
    uint8_t chunk[4096];

    for (;;) {
        // Try to extract a complete frame from the reassembly buffer.
        if (s_rx_buf.size() >= kFrameHeaderSize) {
            const uint32_t len = static_cast<uint32_t>(s_rx_buf[0])
                               | (static_cast<uint32_t>(s_rx_buf[1]) << 8)
                               | (static_cast<uint32_t>(s_rx_buf[2]) << 16)
                               | (static_cast<uint32_t>(s_rx_buf[3]) << 24);
            if (len > kMaxFramePayload) {
                s_rx_buf.clear();
                return RecvResult::Error;
            }
            if (s_rx_buf.size() >= kFrameHeaderSize + len) {
                out.assign(s_rx_buf.begin() + kFrameHeaderSize,
                           s_rx_buf.begin() + kFrameHeaderSize + len);
                s_rx_buf.erase(s_rx_buf.begin(), s_rx_buf.begin() + kFrameHeaderSize + len);
                return RecvResult::Frame;
            }
        }

        int n = sock.recv(chunk, sizeof(chunk));
        if (n > 0) {
            s_rx_buf.insert(s_rx_buf.end(), chunk, chunk + n);
            continue;
        }
        if (n == 0) return RecvResult::Closed;

        if (socket_would_block()) {
            int r = sock.wait_readable(20);
            if (r == 1) continue;          // data arrived, keep assembling
            if (r == 0) return RecvResult::Timeout;
            return RecvResult::Error;
        }
        return RecvResult::Error;
    }
}

// Writes one length-prefixed frame to a non-blocking TCP socket, waiting for
// writability on EAGAIN. Returns false when the connection is gone.
inline bool write_frame(TcpSocket& sock, const void* data, size_t len) {
    if (len > kMaxFramePayload) return false;

    const uint8_t hdr[kFrameHeaderSize] = {
        static_cast<uint8_t>(len & 0xFF),
        static_cast<uint8_t>((len >> 8) & 0xFF),
        static_cast<uint8_t>((len >> 16) & 0xFF),
        static_cast<uint8_t>((len >> 24) & 0xFF)
    };
    const uint8_t* bytes = static_cast<const uint8_t*>(data);

    auto send_fully = [&sock](const uint8_t* p, size_t n) -> bool {
        size_t off = 0;
        while (off < n) {
            int sent = sock.send(p + off, n - off);
            if (sent > 0) {
                off += static_cast<size_t>(sent);
                continue;
            }
            if (sent == 0) return false;
            if (socket_would_block()) {
                if (sock.wait_writable(100) != 1) return false;
                continue;
            }
            return false;
        }
        return true;
    };

    return send_fully(hdr, sizeof(hdr)) && send_fully(bytes, len);
}

} // namespace tunnel
} // namespace audiorouter
