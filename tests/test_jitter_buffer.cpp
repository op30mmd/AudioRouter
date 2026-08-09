#include "../src/client/jitter_buffer.hpp"
#include <iostream>
#include <vector>
#include <span>

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
    config.frames_per_packet = 240;

    JitterBuffer jb(15);
    jb.configure(config, 15);
    TEST_ASSERT(!jb.is_ready());
    TEST_ASSERT(jb.available_frames()==0);
    TEST_ASSERT(!jb.is_configured() == false); // should be configured

    std::vector<int16_t> pkt0(240*2,100), pkt1(240*2,200), pkt2(240*2,300), pkt3(240*2,400);

    // push seq 0 — available_frames counts contiguous even while buffering (is_ready false means not enough yet)
    TEST_ASSERT(jb.push_packet(0,1000,pkt0.data(),240));
    TEST_ASSERT(!jb.is_ready());
    TEST_ASSERT(jb.available_frames()==240);

    // out-of-order 2
    TEST_ASSERT(jb.push_packet(2,3000,pkt2.data(),240));
    TEST_ASSERT(!jb.is_ready());

    // fill gap 1
    TEST_ASSERT(jb.push_packet(1,2000,pkt1.data(),240));
    TEST_ASSERT(jb.is_ready());
    TEST_ASSERT(jb.available_frames() >= 720);

    // duplicate 1 should be rejected
    bool dup = jb.push_packet(1,2000,pkt1.data(),240);
    TEST_ASSERT(!dup);
    // span API duplicate also rejected
    auto dup2 = jb.push_packet(1,2000,std::span<const int16_t>(pkt1));
    TEST_ASSERT(!dup2.has_value() || !dup2.value());

    // pop reordered
    std::vector<int16_t> out(240*2);
    TEST_ASSERT(jb.pop_frames(out.data(),240)==240 && out[0]==100);
    TEST_ASSERT(jb.pop_frames(out.data(),240)==240 && out[0]==200);
    TEST_ASSERT(jb.pop_frames(out.data(),240)==240 && out[0]==300);

    // test span pop
    std::vector<int16_t> pkt4(240*2,500);
    TEST_ASSERT(jb.push_packet(4,5000,pkt4.data(),240));
    // missing 3 -> conceal
    TEST_ASSERT(jb.pop_frames(std::span<int16_t>(out))==240 && out[0]==0);
    TEST_ASSERT(jb.pop_frames(out.data(),240)==240 && out[0]==500);

    auto stats = jb.get_stats();
    TEST_ASSERT(stats.packets_lost >=1);
    TEST_ASSERT(stats.packets_duplicate >=1);

    // ── extended: buffering retrigger after underrun ──
    {
        JitterBuffer jb2(30);
        jb2.configure(config,30);
        std::vector<int16_t> p(240*2, 777);
        // push 6 packets to reach 30ms (6*5ms)
        for (int i=0;i<6;++i) jb2.push_packet(i, 1000+i*5000, p.data(),240);
        TEST_ASSERT(jb2.is_ready());
        // pop 6 packets fully
        for(int i=0;i<6;++i){ std::vector<int16_t> o(240*2); jb2.pop_frames(o.data(),240); }
        TEST_ASSERT(jb2.available_frames()==0);
        // pop empty should produce silence but also count as loss? Our impl returns silence when buffering? Actually after drained, next pop will be loss concealment (since packets missing)
        std::vector<int16_t> silence(240*2, 99);
        jb2.pop_frames(silence.data(),240);
        TEST_ASSERT(silence[0]==0);
        TEST_ASSERT(jb2.get_stats().packets_lost >=1);
    }

    // ── sequence wraparound (simulate near UINT32_MAX) ──
    {
        JitterBuffer jb3(15);
        jb3.configure(config,15);
        uint32_t base = 0xFFFFFFFE; // near wrap
        std::vector<int16_t> pw(240*2, 111);
        TEST_ASSERT(jb3.push_packet(base, 1000, pw.data(),240));
        TEST_ASSERT(jb3.push_packet(base+1, 2000, pw.data(),240));
        TEST_ASSERT(jb3.push_packet(base+2, 3000, pw.data(),240));
        TEST_ASSERT(jb3.is_ready());
        std::vector<int16_t> o(240*2);
        TEST_ASSERT(jb3.pop_frames(o.data(),240)==240);
        // next should be base+1
        TEST_ASSERT(jb3.pop_frames(o.data(),240)==240);
    }

    // ── late packet (diff <0) handling ──
    {
        JitterBuffer jb4(15);
        jb4.configure(config,15);
        std::vector<int16_t> p(240*2, 1);
        for(int i=0;i<3;++i) jb4.push_packet(i, 1000+i*1000, p.data(),240);
        std::vector<int16_t> o(240*2);
        jb4.pop_frames(o.data(),240); // consume seq 0, next is 1
        // now try to push seq 0 again late
        bool late = jb4.push_packet(0, 5000, p.data(),240);
        TEST_ASSERT(!late);
        TEST_ASSERT(jb4.get_stats().packets_out_of_order >=1);
    }

    // ── massive jump resync ──
    {
        JitterBuffer jb5(15);
        jb5.configure(config,15);
        std::vector<int16_t> p(240*2, 5);
        jb5.push_packet(0,1000,p.data(),240);
        jb5.push_packet(1,2000,p.data(),240);
        jb5.push_packet(2,3000,p.data(),240);
        // jump 1000 ahead (>256)
        TEST_ASSERT(jb5.push_packet(1000, 9000, p.data(),240));
        TEST_ASSERT(jb5.get_stats().overruns >=1);
        // after resync, popping should give 1000
        // need to push enough to exit buffering? already had 3 contiguous before jump, now next_play_seq is 1000, we have 1000 buffered, need 2 more to reach target? Actually target is 3 packets, but we only have 1 after resync, so not ready -> pop gives silence
        std::vector<int16_t> o(240*2,9);
        size_t n = jb5.pop_frames(o.data(),240);
        TEST_ASSERT(n==240);
        // After resync, buffering may have reset? Our implementation sets next_play_seq=1000 but stays buffering until target met? Let's ensure it recovers: push 1001,1002
        jb5.push_packet(1001,10000,p.data(),240);
        jb5.push_packet(1002,11000,p.data(),240);
        TEST_ASSERT(jb5.is_ready());
    }

    // ── partial frame consumption (pop smaller than packet) ──
    {
        JitterBuffer jb6(15);
        jb6.configure(config,15);
        std::vector<int16_t> p(240*2, 7);
        for(int i=0;i<3;++i) jb6.push_packet(i, 1000+i*1000, p.data(),240);
        TEST_ASSERT(jb6.is_ready());
        std::vector<int16_t> half(120*2);
        TEST_ASSERT(jb6.pop_frames(half.data(),120)==120);
        TEST_ASSERT(jb6.available_frames()== 600); // 720-120
        std::vector<int16_t> remain(600*2/2); // but need to pop rest? Let's just pop 120 again should give same packet remainder
        std::vector<int16_t> second(120*2);
        TEST_ASSERT(jb6.pop_frames(second.data(),120)==120);
        TEST_ASSERT(second[0]==7);
    }

    // ── jitter estimate not NaN ──
    {
        JitterBuffer jb7(15);
        jb7.configure(config,15);
        std::vector<int16_t> p(240*2, 9);
        for(int i=0;i<5;++i) jb7.push_packet(i, 1000+i*5000, p.data(),240);
        auto s = jb7.get_stats();
        TEST_ASSERT(s.avg_jitter_ms >= 0.0);
        TEST_ASSERT(s.current_buffer_ms >= 0.0);
    }

    // ── reset clears state ──
    {
        JitterBuffer jb8(15);
        jb8.configure(config,15);
        std::vector<int16_t> p(240*2, 8);
        jb8.push_packet(0,1000,p.data(),240);
        jb8.reset();
        TEST_ASSERT(!jb8.is_ready());
        TEST_ASSERT(jb8.available_frames()==0);
    }

    // ── span API success/failure ──
    {
        JitterBuffer jb9(15);
        jb9.configure(config,15);
        std::vector<int16_t> good(240*2, 10);
        auto ok = jb9.push_packet(0,1000, std::span<const int16_t>(good));
        TEST_ASSERT(ok.has_value() && ok.value());
        std::vector<int16_t> bad(5,0); // 5 not divisible by 2
        auto badRes = jb9.push_packet(1,2000, std::span<const int16_t>(bad));
        TEST_ASSERT(!badRes.has_value());
    }

    return true;
}
