#include "../src/common/socket_util.hpp"
#include <iostream>
#include <vector>
#include <cstring>
#include <span>
#include <array>

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        return false; \
    } \
} while(0)

bool run_socket_tests() {
    using namespace audiorouter;

    UdpSocket::init_networking();

    // Address parsing exhaustive
    SocketAddress addr1("127.0.0.1", 54321);
    TEST_ASSERT(addr1.is_valid());
    TEST_ASSERT(addr1.ip() == "127.0.0.1");
    TEST_ASSERT(addr1.port() == 54321);
    TEST_ASSERT(addr1.to_string() == "127.0.0.1:54321");
    // create factory
    auto created = SocketAddress::create("127.0.0.1", 8080);
    TEST_ASSERT(created.has_value() && created->port()==8080);
    auto bad = SocketAddress::create("999.999.999.999", 80);
    TEST_ASSERT(!bad.has_value());

    SocketAddress any("0.0.0.0", 0);
    TEST_ASSERT(any.is_valid() && any.ip()=="0.0.0.0");
    SocketAddress bcast("255.255.255.255", 44100);
    TEST_ASSERT(bcast.is_valid());
    SocketAddress empty("", 1234);
    TEST_ASSERT(empty.is_valid()); // empty treated as INADDR_ANY per impl

    SocketAddress addr2("127.0.0.1", 54321);
    TEST_ASSERT(addr1 == addr2);
    SocketAddress addr3("192.168.1.100", 8080);
    TEST_ASSERT(addr1 != addr3);
    // invalid vs invalid
    SocketAddress inv1, inv2;
    TEST_ASSERT(!inv1.is_valid());
    TEST_ASSERT(inv1 == inv2); // both invalid equal per impl? check
    // copy via sockaddr_in
    struct sockaddr_in raw = addr1.raw();
    SocketAddress from_raw(raw);
    TEST_ASSERT(from_raw.is_valid() && from_raw == addr1);

    // get_local_interfaces not empty on linux (at least loopback)
    auto ifaces = UdpSocket::get_local_interfaces();
    TEST_ASSERT(!ifaces.empty());
    bool has_loop=false;
    for(auto &i: ifaces) if(i.is_loopback) has_loop=true;
    TEST_ASSERT(has_loop);

    // UDP loopback via raw API
    UdpSocket s1, s2;
    TEST_ASSERT(s1.open() && s2.open());
    TEST_ASSERT(s1.bind(0, "127.0.0.1"));
    TEST_ASSERT(s2.bind(0, "127.0.0.1"));
    SocketAddress s1_addr = s1.get_local_address();
    SocketAddress s2_addr = s2.get_local_address();
    TEST_ASSERT(s1_addr.is_valid() && s2_addr.is_valid());
    TEST_ASSERT(s1_addr.port() != 0 && s2_addr.port() != 0);
    s2.set_receive_timeout_ms(1000);
    s1.set_send_timeout_ms(1000);
    const char test_msg[] = "Hello AudioRouter UDP!";
    int sent = s1.send_to(test_msg, sizeof(test_msg), s2_addr);
    TEST_ASSERT(sent == sizeof(test_msg));
    char recv_buf[128]={0};
    SocketAddress sender;
    int recvd = s2.receive_from(recv_buf, sizeof(recv_buf), sender);
    TEST_ASSERT(recvd == sizeof(test_msg));
    TEST_ASSERT(std::strcmp(recv_buf, test_msg)==0);
    TEST_ASSERT(sender.port()==s1_addr.port());
    TEST_ASSERT(sender.ip()=="127.0.0.1");

    // span-based API
    {
        UdpSocket a,b;
        a.open(); b.open();
        a.bind(0,"127.0.0.1"); b.bind(0,"127.0.0.1");
        auto ba = a.get_local_address(); auto bb = b.get_local_address();
        b.set_receive_timeout_ms(1000);
        std::array<std::byte, 4> payload{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
        auto sres = a.send_to(std::span<const std::byte>(payload), bb);
        TEST_ASSERT(sres.has_value() && *sres==4);
        std::array<std::byte, 16> recv{};
        SocketAddress from;
        auto rres = b.receive_from(std::span<std::byte>(recv), from);
        TEST_ASSERT(rres.has_value() && *rres==4);
        TEST_ASSERT(recv[0]==std::byte{0xDE});
        TEST_ASSERT(from == ba);
        // empty span should fail
        auto empty_res = a.send_to(std::span<const std::byte>{}, bb);
        TEST_ASSERT(!empty_res.has_value());
        // invalid dest
        SocketAddress inv;
        auto inv_res = a.send_to(std::span<const std::byte>(payload), inv);
        TEST_ASSERT(!inv_res.has_value());
        a.close(); b.close();
    }

    // socket options
    {
        UdpSocket sock;
        TEST_ASSERT(sock.open());
        TEST_ASSERT(sock.set_broadcast(true));
        TEST_ASSERT(sock.set_non_blocking(true));
        TEST_ASSERT(sock.set_non_blocking(false));
        TEST_ASSERT(sock.set_buffer_sizes(64*1024, 64*1024));
        TEST_ASSERT(!sock.set_buffer_sizes(100*1024*1024, 0)); // too large rejected
        TEST_ASSERT(!sock.set_receive_timeout_ms(-1));
        TEST_ASSERT(sock.set_receive_timeout_ms(100));
        TEST_ASSERT(sock.set_qos_priority(true));
        TEST_ASSERT(sock.set_qos_priority(false));
        sock.close();
        // operations on closed socket: raw API auto-opens, span API should report error
        {
            UdpSocket closed;
            // raw API will auto-open on send, so we check that it reopens rather than failing
            SocketAddress dummy("127.0.0.1", 9999);
            // closed socket initially not open
            TEST_ASSERT(!closed.is_open());
            int r = closed.send_to("hi",2,dummy);
            TEST_ASSERT(r==-1 || r==2); // raw may auto-open and succeed, or fail depending on network; either is acceptable but shouldn't crash
            // span API on non-open socket should return error when using empty check? Actually span send_to will auto-open as well, so may succeed.
            // Test that span API with invalid dest fails
            SocketAddress inv;
            std::array<std::byte,2> payload{std::byte{0}, std::byte{1}};
            auto sr = closed.send_to(std::span<const std::byte>(payload), inv);
            TEST_ASSERT(!sr.has_value());
            // receive on closed (no open) should fail
            UdpSocket neverOpened;
            std::array<std::byte,10> buf2{};
            SocketAddress src2;
            auto rr = neverOpened.receive_from(std::span<std::byte>(buf2), src2);
            TEST_ASSERT(!rr.has_value());
        }
    }

    // bind failure with invalid ip
    {
        UdpSocket s;
        s.open();
        TEST_ASSERT(!s.bind(1234, "999.999.999.999")); // invalid ip should make bind fail
        s.close();
    }

    // large payload handling (1400 MTU)
    {
        UdpSocket a,b;
        a.open(); b.open();
        a.bind(0,"127.0.0.1"); b.bind(0,"127.0.0.1");
        b.set_receive_timeout_ms(1000);
        auto bb = b.get_local_address();
        std::vector<std::byte> large(1400, std::byte{0xAB});
        auto sres = a.send_to(std::span<const std::byte>(large), bb);
        TEST_ASSERT(sres.has_value());
        std::vector<std::byte> recv(2000, std::byte{0});
        SocketAddress from;
        auto rres = b.receive_from(std::span<std::byte>(recv), from);
        TEST_ASSERT(rres.has_value() && *rres==1400);
        a.close(); b.close();
    }

    // move semantics
    {
        UdpSocket orig;
        orig.open();
        orig.bind(0,"127.0.0.1");
        auto addr = orig.get_local_address();
        UdpSocket moved(std::move(orig));
        TEST_ASSERT(!orig.is_open());
        TEST_ASSERT(moved.is_open());
        TEST_ASSERT(moved.get_local_address() == addr);
        UdpSocket assigned;
        assigned = std::move(moved);
        TEST_ASSERT(!moved.is_open());
        TEST_ASSERT(assigned.is_open());
    }

    s1.close(); s2.close();
    return true;
}
