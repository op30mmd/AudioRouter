#include "../src/client/jitter_buffer.hpp"
#include <iostream>
#include <vector>

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        return false; \
    } \
} while(0)

bool run_jitter_buffer_tests() {
    using namespace audiorouter;

    AudioConfig config;
    config.sample_rate = 48000;
    config.channels = 2;
    config.format = AudioSampleFormat::PCM_S16LE;
    config.frames_per_packet = 240; // 5ms

    JitterBuffer jb(15); // 15ms target buffer depth = 3 packets (720 frames)
    jb.configure(config, 15);

    TEST_ASSERT(!jb.is_ready());

    // Create sample packets
    std::vector<int16_t> pkt0(240 * 2, 100);
    std::vector<int16_t> pkt1(240 * 2, 200);
    std::vector<int16_t> pkt2(240 * 2, 300);
    std::vector<int16_t> pkt3(240 * 2, 400);

    // Push packet 0
    jb.push_packet(0, 1000, pkt0.data(), 240);
    TEST_ASSERT(!jb.is_ready()); // Needs 3 packets (15ms)

    // Push packet 2 (out of order, before packet 1!)
    jb.push_packet(2, 3000, pkt2.data(), 240);
    TEST_ASSERT(!jb.is_ready());

    // Push packet 1 (filling the gap!)
    jb.push_packet(1, 2000, pkt1.data(), 240);
    TEST_ASSERT(!jb.is_ready()); // First fill needs the startup prefill (250ms = 50 packets)

    // Push packets 3..49 to satisfy the startup prefill (3 contiguous slots
    // are enough for the steady-state target, but the first fill is larger).
    std::vector<int16_t> pkt_extra(240 * 2, 350);
    for (uint32_t seq = 3; seq < 50; ++seq) {
        jb.push_packet(seq, 1000 + seq * 1000, pkt_extra.data(), 240);
    }
    TEST_ASSERT(jb.is_ready()); // Startup prefill complete (50 contiguous packets)

    // Push duplicate packet 1 (should be ignored)
    bool dup_res = jb.push_packet(1, 2000, pkt1.data(), 240);
    TEST_ASSERT(!dup_res);

    // Pop frames from jitter buffer
    std::vector<int16_t> out(240 * 2);
    size_t pop1 = jb.pop_frames(out.data(), 240);
    TEST_ASSERT(pop1 == 240);
    TEST_ASSERT(out[0] == 100); // Packet 0

    size_t pop2 = jb.pop_frames(out.data(), 240);
    TEST_ASSERT(pop2 == 240);
    TEST_ASSERT(out[0] == 200); // Packet 1 (reordered properly!)

    size_t pop3 = jb.pop_frames(out.data(), 240);
    TEST_ASSERT(pop3 == 240);
    TEST_ASSERT(out[0] == 300); // Packet 2

    // Pop the rest of the startup prefill (packets 3..49 = 350)
    for (uint32_t seq = 3; seq < 50; ++seq) {
        size_t pop_extra = jb.pop_frames(out.data(), 240);
        TEST_ASSERT(pop_extra == 240);
        TEST_ASSERT(out[0] == 350);
    }

    // Now the buffer is drained and the play pointer is at packet 50.
    // Test packet loss concealment: packet 50 is missing, we push packet 51!
    std::vector<int16_t> pkt51(240 * 2, 500);
    jb.push_packet(51, 5000, pkt51.data(), 240);

    // Popping should conceal missing packet 50 with silence (0)
    size_t pop_lost = jb.pop_frames(out.data(), 240);
    TEST_ASSERT(pop_lost == 240);
    TEST_ASSERT(out[0] == 0); // Concealed with silence

    // Next pop should return packet 51
    size_t pop5 = jb.pop_frames(out.data(), 240);
    TEST_ASSERT(pop5 == 240);
    TEST_ASSERT(out[0] == 500); // Packet 51

    auto stats = jb.get_stats();
    TEST_ASSERT(stats.packets_lost == 1);
    TEST_ASSERT(stats.packets_duplicate >= 1);

    // Now the buffer is completely empty (starved).
    // Test that popping repeatedly during starvation delivers silence without running away with lost_packets!
    for (int i = 0; i < 50; ++i) {
        size_t starv_pop = jb.pop_frames(out.data(), 240);
        TEST_ASSERT(starv_pop == 240);
        TEST_ASSERT(out[0] == 0);
    }

    auto stats_after_starv = jb.get_stats();
    // packets_lost should STILL be 1 (NOT 51)!
    TEST_ASSERT(stats_after_starv.packets_lost == 1);

    // Stream resumes: sender sends packet 52, 53, 54!
    std::vector<int16_t> pkt52(240 * 2, 600);
    std::vector<int16_t> pkt53(240 * 2, 700);
    std::vector<int16_t> pkt54(240 * 2, 800);

    TEST_ASSERT(jb.push_packet(52, 6000, pkt52.data(), 240));
    TEST_ASSERT(jb.push_packet(53, 7000, pkt53.data(), 240));
    TEST_ASSERT(jb.push_packet(54, 8000, pkt54.data(), 240));

    // Jitter buffer should be ready again (steady-state target is only 3
    // packets now) and play packet 52 without dropping it as late
    TEST_ASSERT(jb.is_ready());

    size_t pop_resumed52 = jb.pop_frames(out.data(), 240);
    TEST_ASSERT(pop_resumed52 == 240);
    TEST_ASSERT(out[0] == 600);

    size_t pop_resumed53 = jb.pop_frames(out.data(), 240);
    TEST_ASSERT(pop_resumed53 == 240);
    TEST_ASSERT(out[0] == 700);

    size_t pop_resumed54 = jb.pop_frames(out.data(), 240);
    TEST_ASSERT(pop_resumed54 == 240);
    TEST_ASSERT(out[0] == 800);

    auto stats_final = jb.get_stats();
    TEST_ASSERT(stats_final.packets_lost == 1);

    return true;
}
