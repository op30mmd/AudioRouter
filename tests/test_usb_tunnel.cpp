#include "../src/common/socket_util.hpp"
#include "../src/common/usb_tunnel.hpp"
#include "../src/common/time_util.hpp"
#include <iostream>
#include <vector>
#include <cstring>

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        return false; \
    } \
} while(0)

namespace {

using namespace audiorouter;

// Pumps read_frame() until a complete frame arrives or the deadline passes.
bool pump_frame(TcpSocket& sock, std::vector<uint8_t>& out, int timeout_ms) {
    const uint64_t deadline = get_time_ms() + static_cast<uint64_t>(timeout_ms);
    while (get_time_ms() < deadline) {
        auto r = tunnel::read_frame(sock, out);
        if (r == tunnel::RecvResult::Frame) return true;
        if (r == tunnel::RecvResult::Closed || r == tunnel::RecvResult::Error) return false;
        // Timeout: nothing complete yet, keep waiting until the deadline.
    }
    return false;
}

// Serializes a length-prefixed frame the way write_frame() would, so tests
// can send partial frames byte by byte.
std::vector<uint8_t> make_frame(const std::vector<uint8_t>& payload) {
    const size_t len = payload.size();
    std::vector<uint8_t> frame;
    frame.reserve(tunnel::kFrameHeaderSize + len);
    frame.push_back(static_cast<uint8_t>(len & 0xFF));
    frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    frame.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

} // namespace

bool run_usb_tunnel_tests() {
    UdpSocket::init_networking();

    // --- Establish a loopback TCP pair the way the relays do. ---
    TcpSocket listener;
    TcpSocket accepted;
    TcpSocket connector;

    TEST_ASSERT(listener.listen(0, "127.0.0.1", 1));
    const SocketAddress listen_addr = listener.get_local_address();
    TEST_ASSERT(listen_addr.is_valid());
    TEST_ASSERT(listen_addr.port() != 0);

    TEST_ASSERT(connector.connect(listen_addr, 1000));
    connector.set_tcp_nodelay(true);
    connector.set_non_blocking(true);
    TEST_ASSERT(listener.accept(accepted, 1000));
    accepted.set_tcp_nodelay(true);
    accepted.set_non_blocking(true);

    // --- Frame roundtrip over the TCP pair. ---
    tunnel::reset_frame_buffer();
    const std::vector<uint8_t> payload{0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x7F, 0x42, 0xC3, 0x00, 0x99, 0x11};
    TEST_ASSERT(tunnel::write_frame(connector, payload.data(), payload.size()));

    std::vector<uint8_t> out;
    TEST_ASSERT(pump_frame(accepted, out, 2000));
    TEST_ASSERT(out == payload);

    // --- Empty (zero-length) frame. ---
    TEST_ASSERT(tunnel::write_frame(connector, payload.data(), 0));
    TEST_ASSERT(pump_frame(accepted, out, 2000));
    TEST_ASSERT(out.empty());

    // --- Oversized payload is rejected by write_frame. ---
    TEST_ASSERT(!tunnel::write_frame(connector, nullptr, tunnel::kMaxFramePayload + 1));

    // --- Partial header: a frame split across read_frame() calls. ---
    const std::vector<uint8_t> payload2{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    const std::vector<uint8_t> frame2 = make_frame(payload2);
    TEST_ASSERT(connector.send(frame2.data(), 2) == 2);
    sleep_ms(50);  // let the 2 header bytes reach the reassembly buffer
    {
        std::vector<uint8_t> scratch;
        auto r = tunnel::read_frame(accepted, scratch);
        TEST_ASSERT(r == tunnel::RecvResult::Timeout);  // header incomplete
    }
    TEST_ASSERT(connector.send(frame2.data() + 2, frame2.size() - 2) == static_cast<int>(frame2.size() - 2));
    TEST_ASSERT(pump_frame(accepted, out, 2000));
    TEST_ASSERT(out == payload2);

    // --- reset_frame_buffer() drops stale partial data from a dead session. ---
    const std::vector<uint8_t> stale = make_frame({0xAA, 0xBB, 0xCC, 0xDD});
    TEST_ASSERT(connector.send(stale.data(), 2) == 2);
    sleep_ms(50);
    {
        std::vector<uint8_t> scratch;
        TEST_ASSERT(tunnel::read_frame(accepted, scratch) == tunnel::RecvResult::Timeout);
    }
    tunnel::reset_frame_buffer();  // new session: forget the 2 stale bytes
    const std::vector<uint8_t> fresh{0x01, 0x02, 0x03};
    TEST_ASSERT(tunnel::write_frame(connector, fresh.data(), fresh.size()));
    TEST_ASSERT(pump_frame(accepted, out, 2000));
    TEST_ASSERT(out == fresh);

    // --- write_frame fails cleanly on a closed socket. ---
    connector.close();
    TEST_ASSERT(!tunnel::write_frame(connector, payload.data(), payload.size()));

    accepted.close();
    listener.close();
    return true;
}
