#include "../src/common/socket_util.hpp"
#include <iostream>
#include <vector>
#include <cstring>

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        return false; \
    } \
} while(0)

bool run_socket_tests() {
    using namespace audiorouter;

    UdpSocket::init_networking();

    // Test SocketAddress parsing
    SocketAddress addr1("127.0.0.1", 54321);
    TEST_ASSERT(addr1.is_valid());
    TEST_ASSERT(addr1.ip() == "127.0.0.1");
    TEST_ASSERT(addr1.port() == 54321);
    TEST_ASSERT(addr1.to_string() == "127.0.0.1:54321");

    SocketAddress addr2("127.0.0.1", 54321);
    TEST_ASSERT(addr1 == addr2);

    SocketAddress addr3("192.168.1.100", 8080);
    TEST_ASSERT(addr1 != addr3);

    // Test UDP loopback transmission
    UdpSocket s1;
    UdpSocket s2;

    TEST_ASSERT(s1.open());
    TEST_ASSERT(s2.open());

    TEST_ASSERT(s1.bind(0, "127.0.0.1")); // Bind to ephemeral port
    TEST_ASSERT(s2.bind(0, "127.0.0.1"));

    SocketAddress s1_addr = s1.get_local_address();
    SocketAddress s2_addr = s2.get_local_address();

    TEST_ASSERT(s1_addr.is_valid());
    TEST_ASSERT(s2_addr.is_valid());

    s2.set_receive_timeout_ms(1000);

    const char test_msg[] = "Hello AudioRouter UDP!";
    int sent = s1.send_to(test_msg, sizeof(test_msg), s2_addr);
    TEST_ASSERT(sent == sizeof(test_msg));

    char recv_buf[128] = {0};
    SocketAddress sender;
    int received = s2.receive_from(recv_buf, sizeof(recv_buf), sender);
    TEST_ASSERT(received == sizeof(test_msg));
    TEST_ASSERT(std::strcmp(recv_buf, test_msg) == 0);
    TEST_ASSERT(sender.port() == s1_addr.port());

    s1.close();
    s2.close();
    return true;
}
