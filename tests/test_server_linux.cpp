#include "../src/server/pulse_capture.hpp"
#include "../src/server/alsa_capture.hpp"
#include "../src/server/audio_endpoint_control.hpp"
#include "../src/server/server.hpp"
#include "../src/common/protocol.hpp"
#include "../src/common/socket_util.hpp"
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

bool run_server_linux_tests() {
    using namespace audiorouter;

    // 1. Device spec parsing for PulseCapture
    TEST_ASSERT(PulseCapture::parse_source_spec("") == "");
    TEST_ASSERT(PulseCapture::parse_source_spec("auto") == "");
    TEST_ASSERT(PulseCapture::parse_source_spec("pulse") == "");
    TEST_ASSERT(PulseCapture::parse_source_spec("pulseaudio") == "");
    TEST_ASSERT(PulseCapture::parse_source_spec("pa") == "");
    TEST_ASSERT(PulseCapture::parse_source_spec("pulse:default") == "");
    TEST_ASSERT(PulseCapture::parse_source_spec("pulse:@DEFAULT_MONITOR@") == "");
    TEST_ASSERT(PulseCapture::parse_source_spec("pulse:alsa_output.pci.monitor") == "alsa_output.pci.monitor");
    TEST_ASSERT(PulseCapture::parse_source_spec("pa:my_sink.monitor") == "my_sink.monitor");

    // 2. Device spec parsing for AlsaCapture
    TEST_ASSERT(AlsaCapture::parse_device_spec("") == "default");
    TEST_ASSERT(AlsaCapture::parse_device_spec("auto") == "default");
    TEST_ASSERT(AlsaCapture::parse_device_spec("alsa") == "default");
    TEST_ASSERT(AlsaCapture::parse_device_spec("alsa:") == "default");
    TEST_ASSERT(AlsaCapture::parse_device_spec("alsa:hw:Loopback,1,0") == "hw:Loopback,1,0");

    // 3. Runtime library probes should never crash
    (void)PulseCapture::is_supported();
    (void)PulseCapture::server_available();
    (void)AlsaCapture::is_supported();

    // 4. AudioEndpointControl safe lifecycle even when no hardware mixer exists
    {
        AudioEndpointControl ep;
        bool inited = ep.init();
        if (!inited) {
            TEST_ASSERT(!ep.is_currently_silenced_by_us());
        }
        ep.shutdown();
    }

    // 5. AudioRouterServer start/stop + discovery handshake on loopback with test tone
    {
        // Find a free UDP port by binding an ephemeral socket and reading its port
        UdpSocket probe;
        TEST_ASSERT(probe.open());
        TEST_ASSERT(probe.bind(0, "127.0.0.1"));
        const uint16_t port = probe.get_local_address().port();
        probe.close();
        TEST_ASSERT(port != 0);

        ServerConfig cfg;
        cfg.port = port;
        cfg.bind_ip = "127.0.0.1";
        cfg.auto_mute_pc_speaker = false;
        cfg.use_test_tone = true;

        AudioRouterServer srv(cfg);
        TEST_ASSERT(srv.start());
        TEST_ASSERT(srv.is_running());
        TEST_ASSERT(srv.get_state() == ServerState::LISTENING);

        // Send a DISCOVERY_REQ from a local UDP client
        UdpSocket cli;
        TEST_ASSERT(cli.open());
        TEST_ASSERT(cli.bind(0, "127.0.0.1"));
        cli.set_receive_timeout_ms(1000);

        std::vector<uint8_t> req(sizeof(protocol::CommonHeader) + sizeof(protocol::DiscoveryReqPayload), 0);
        auto* hdr = reinterpret_cast<protocol::CommonHeader*>(req.data());
        hdr->magic = protocol::MAGIC;
        hdr->version = protocol::CURRENT_VERSION;
        hdr->msg_type = static_cast<uint8_t>(protocol::MsgType::DISCOVERY_REQ);
        hdr->flags = protocol::FLAG_NONE;
        hdr->seq_num = 1;
        hdr->timestamp_us = get_time_us();
        hdr->payload_size = sizeof(protocol::DiscoveryReqPayload);

        SocketAddress srv_addr("127.0.0.1", port);
        TEST_ASSERT(cli.send_to(req.data(), req.size(), srv_addr) > 0);

        std::vector<uint8_t> resp(512, 0);
        SocketAddress from;
        int n = cli.receive_from(resp.data(), resp.size(), from);
        TEST_ASSERT(n >= static_cast<int>(sizeof(protocol::CommonHeader) + sizeof(protocol::DiscoveryRespPayload)));

        const auto* resp_hdr = reinterpret_cast<const protocol::CommonHeader*>(resp.data());
        TEST_ASSERT(protocol::is_valid_header(*resp_hdr, static_cast<size_t>(n)));
        TEST_ASSERT(static_cast<protocol::MsgType>(resp_hdr->msg_type) == protocol::MsgType::DISCOVERY_RESP);

        const auto* resp_pay = reinterpret_cast<const protocol::DiscoveryRespPayload*>(
            resp.data() + sizeof(protocol::CommonHeader));
        TEST_ASSERT(resp_pay->server_port == port);
        TEST_ASSERT(resp_pay->is_busy == 0);

        cli.close();
        srv.stop();
        TEST_ASSERT(!srv.is_running());
        TEST_ASSERT(srv.get_state() == ServerState::STOPPED);
    }

    std::cout << "  [server_linux] Pulse/ALSA spec parsing, endpoint control, and server discovery OK\n";
    return true;
}
