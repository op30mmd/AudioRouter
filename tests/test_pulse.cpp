#include "../src/client/pulse_player.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        return false; \
    } \
} while(0)

bool run_pulse_tests() {
    using namespace audiorouter::pulse;

    // ---- device name recognition ----
    {
        TEST_ASSERT(is_pulse_device_name("pulse"));
        TEST_ASSERT(is_pulse_device_name("pulseaudio"));
        TEST_ASSERT(is_pulse_device_name("pa"));
        TEST_ASSERT(is_pulse_device_name("pulse:alsa_output.analog-stereo"));
        TEST_ASSERT(is_pulse_device_name("pulse@40"));
        TEST_ASSERT(is_pulse_device_name("pa:my_sink@25"));

        TEST_ASSERT(!is_pulse_device_name("default"));
        TEST_ASSERT(!is_pulse_device_name("hw:0,0"));
        TEST_ASSERT(!is_pulse_device_name("aaudio"));
        TEST_ASSERT(!is_pulse_device_name("termux"));
        TEST_ASSERT(!is_pulse_device_name("/dev/snd/pcmC0D0p"));
        // A prefix match must not swallow unrelated names.
        TEST_ASSERT(!is_pulse_device_name("pulsex"));
        TEST_ASSERT(!is_pulse_device_name("path"));
    }

    // ---- device spec parsing ----
    {
        auto s = parse_device_spec("pulse");
        TEST_ASSERT(s.sink.empty());
        TEST_ASSERT(s.latency_ms == 0);

        s = parse_device_spec("pulseaudio");
        TEST_ASSERT(s.sink.empty() && s.latency_ms == 0);

        s = parse_device_spec("pulse:alsa_output.pci-0000_00_1f.3.analog-stereo");
        TEST_ASSERT(s.sink == "alsa_output.pci-0000_00_1f.3.analog-stereo");
        TEST_ASSERT(s.latency_ms == 0);

        s = parse_device_spec("pulse@40");
        TEST_ASSERT(s.sink.empty());
        TEST_ASSERT(s.latency_ms == 40);

        s = parse_device_spec("pulse:my_sink@25");
        TEST_ASSERT(s.sink == "my_sink");
        TEST_ASSERT(s.latency_ms == 25);

        s = parse_device_spec("pa:my_sink@25");
        TEST_ASSERT(s.sink == "my_sink" && s.latency_ms == 25);

        // "default" and @DEFAULT_SINK@ mean "let the daemon choose".
        s = parse_device_spec("pulse:default");
        TEST_ASSERT(s.sink.empty());
        s = parse_device_spec("pulse:@DEFAULT_SINK@");
        TEST_ASSERT(s.sink.empty());
        s = parse_device_spec("pulse:@DEFAULT_SINK@@30");
        TEST_ASSERT(s.sink.empty() && s.latency_ms == 30);

        // A non-numeric tail is part of the sink name, not a latency.
        s = parse_device_spec("pulse:sink@host");
        TEST_ASSERT(s.sink == "sink@host");
        TEST_ASSERT(s.latency_ms == 0);

        // Out-of-range latencies clamp into [kMinLatencyMs, kMaxLatencyMs].
        s = parse_device_spec("pulse@1");
        TEST_ASSERT(s.latency_ms == kMinLatencyMs);
        s = parse_device_spec("pulse@100000");
        TEST_ASSERT(s.latency_ms == kMaxLatencyMs);

        // Not a pulse device: neutral spec, never a crash.
        s = parse_device_spec("hw:0,0");
        TEST_ASSERT(s.sink.empty() && s.latency_ms == 0);
        s = parse_device_spec("");
        TEST_ASSERT(s.sink.empty() && s.latency_ms == 0);
    }

    // ---- buffer sizing: derived latency (48 kHz stereo S16, 240-frame packets) ----
    {
        const uint32_t rate = 48000;
        const size_t bpf = 4;              // 2 ch * 2 bytes
        const uint32_t fpp = 240;          // 5 ms
        const auto a = compute_buffer_attr(rate, bpf, fpp, 0);

        const uint32_t packet_bytes = fpp * static_cast<uint32_t>(bpf);  // 960
        TEST_ASSERT(a.minreq == packet_bytes);
        TEST_ASSERT(a.prebuf == packet_bytes);
        // 4 packets = 20 ms, but the floor is kMinLatencyMs = 10 ms; 20 ms wins.
        TEST_ASSERT(a.tlength == rate * bpf * 20 / 1000);   // 3840 bytes
        TEST_ASSERT(a.maxlength == a.tlength * 2);
        TEST_ASSERT(a.tlength % bpf == 0);
        TEST_ASSERT(a.maxlength >= a.tlength);
        TEST_ASSERT(a.tlength >= a.minreq);
    }

    // ---- buffer sizing: explicit latency ----
    {
        const auto a = compute_buffer_attr(48000, 4, 240, 100);
        TEST_ASSERT(a.tlength == 48000u * 4u * 100u / 1000u);  // 19200 bytes
        TEST_ASSERT(a.minreq == 960);
        TEST_ASSERT(a.maxlength == a.tlength * 2);
    }

    // ---- buffer sizing: a latency below one packet still fits a whole write ----
    {
        // 10 ms floor vs a 50 ms packet: tlength must not fall under one packet.
        const auto a = compute_buffer_attr(48000, 4, 2400 /* 50 ms */, 10);
        TEST_ASSERT(a.tlength >= a.minreq);
        TEST_ASSERT(a.minreq == 2400u * 4u);
    }

    // ---- buffer sizing: float32 / mono / odd rates stay frame-aligned ----
    {
        const auto a = compute_buffer_attr(44100, 4 /* mono float32 */, 441, 0);
        TEST_ASSERT(a.tlength % 4 == 0);
        TEST_ASSERT(a.minreq == 441u * 4u);
        TEST_ASSERT(a.tlength >= a.minreq);
    }

    // ---- buffer sizing: degenerate inputs are rejected, not divided by ----
    {
        auto a = compute_buffer_attr(0, 4, 240, 0);
        TEST_ASSERT(a.tlength == 0 && a.maxlength == 0 && a.minreq == 0 && a.prebuf == 0);
        a = compute_buffer_attr(48000, 0, 240, 0);
        TEST_ASSERT(a.tlength == 0 && a.maxlength == 0);
        // frames_per_packet == 0 falls back to the 240-frame server default.
        a = compute_buffer_attr(48000, 4, 0, 0);
        TEST_ASSERT(a.minreq == 960);
        TEST_ASSERT(a.tlength > 0);
    }

    // ---- the stub/real backend contract holds either way ----
    {
        audiorouter::PulsePlayer player;
        TEST_ASSERT(!player.is_open());
        TEST_ASSERT(player.get_buffer_delay_frames() == 0);
        TEST_ASSERT(player.write_frames(nullptr, 0) == 0);
        player.flush();   // must be a no-op on a closed player
        player.close();   // idempotent
        TEST_ASSERT(!player.is_open());
        TEST_ASSERT(player.get_device_name() == "pulse");
    }

    // ---- server_available(): a socket owned by another uid is not usable ----
    // Regression test for the on-device failure where the client ran as root
    // (via su, for -b/--bind) while the PulseAudio daemon ran as the Termux
    // app user: libpulse reported "Connection refused" and the client burned
    // its whole retry budget before falling through to AAudio.
    {
        // A path that does not exist must never be reported as available.
        // (Point every lookup at an empty dir so the host's real daemon, if
        // any, cannot influence the result.)
        char tmpl[] = "/tmp/ar_pulse_test_XXXXXX";
        const char* dir = ::mkdtemp(tmpl);
        TEST_ASSERT(dir != nullptr);

        const std::string saved_server = std::getenv("PULSE_SERVER") ? std::getenv("PULSE_SERVER") : "";
        const std::string saved_xdg = std::getenv("XDG_RUNTIME_DIR") ? std::getenv("XDG_RUNTIME_DIR") : "";
        const std::string saved_prefix = std::getenv("PREFIX") ? std::getenv("PREFIX") : "";
        const std::string saved_home = std::getenv("HOME") ? std::getenv("HOME") : "";

        ::unsetenv("PULSE_SERVER");
        ::setenv("XDG_RUNTIME_DIR", dir, 1);
        ::setenv("PREFIX", dir, 1);
        ::setenv("HOME", dir, 1);

        // No socket anywhere -> unavailable.
        TEST_ASSERT(!audiorouter::PulsePlayer::server_available());

        // An explicit PULSE_SERVER always wins (it may be a TCP endpoint).
        ::setenv("PULSE_SERVER", "127.0.0.1:4713", 1);
        TEST_ASSERT(audiorouter::PulsePlayer::server_available());
        ::unsetenv("PULSE_SERVER");

        // A socket owned by THIS user counts as available.
        const std::string sock_dir = std::string(dir) + "/pulse";
        ::mkdir(sock_dir.c_str(), 0755);
        const std::string sock = sock_dir + "/native";
        FILE* f = std::fopen(sock.c_str(), "w");
        TEST_ASSERT(f != nullptr);
        std::fclose(f);
        // Only meaningful when not running the suite as root: as a normal user
        // the socket is ours, so it must be reported as available.
        if (::getuid() != 0) {
            TEST_ASSERT(audiorouter::PulsePlayer::server_available());
        }

        ::unlink(sock.c_str());
        ::rmdir(sock_dir.c_str());
        ::rmdir(dir);

        if (!saved_server.empty()) ::setenv("PULSE_SERVER", saved_server.c_str(), 1);
        if (!saved_xdg.empty()) ::setenv("XDG_RUNTIME_DIR", saved_xdg.c_str(), 1); else ::unsetenv("XDG_RUNTIME_DIR");
        if (!saved_prefix.empty()) ::setenv("PREFIX", saved_prefix.c_str(), 1); else ::unsetenv("PREFIX");
        if (!saved_home.empty()) ::setenv("HOME", saved_home.c_str(), 1);
    }

    std::cout << "  [pulse] device parsing, buffer sizing, daemon probe and closed-player contract OK\n";
    return true;
}
