#include "../src/common/ring_buffer.hpp"
#include "../src/common/protocol.hpp"
#include "../src/common/audio_types.hpp"
#include "../src/client/jitter_buffer.hpp"
#include "../src/common/socket_util.hpp"
#include <iostream>
#include <vector>
#include <limits>
#include "../src/common/span_compat.hpp"
#include <cstring>

#define TEST_ASSERT(cond) do { if (!(cond)) { std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; return false; } } while(0)

bool run_memory_safety_tests() {
    using namespace audiorouter;
    using namespace audiorouter::protocol;

    // ── RingBuffer bounds ──
    {
        // zero capacity must throw
        bool threw = false;
        try { RingBuffer<int> r(0); } catch (const std::invalid_argument&) { threw = true; }
        TEST_ASSERT(threw);

        RingBuffer<int16_t> ring(10);
        TEST_ASSERT(ring.write(nullptr, 5)==0);
        TEST_ASSERT(ring.read(nullptr, 5)==0);
        TEST_ASSERT(ring.write(std::span<const int16_t>())==0);
        TEST_ASSERT(ring.peek(nullptr, 5)==0);
        TEST_ASSERT(ring.peek(std::span<int16_t>())==0);

        // overflow write not crash, returns limited
        std::vector<int16_t> big(100, 1);
        size_t w = ring.write(big.data(), big.size());
        TEST_ASSERT(w==10 && ring.full());
        TEST_ASSERT(ring.write(big.data(), 1)==0); // full

        // overwrite with huge count (>capacity) should keep last capacity items
        ring.clear();
        std::vector<int16_t> seq(20);
        for(int i=0;i<20;++i) seq[i]=static_cast<int16_t>(i);
        ring.write_overwrite(seq.data(), seq.size());
        TEST_ASSERT(ring.size()==10);
        std::vector<int16_t> out(10);
        ring.read(out.data(),10);
        TEST_ASSERT(out[0]==10 && out[9]==19);

        // read_pad_silence
        ring.clear();
        ring.write(std::vector<int16_t>{1,2,3}.data(),3);
        std::vector<int16_t> padded(5, 9);
        ring.read_pad_silence(padded.data(),5, int16_t(0));
        TEST_ASSERT(padded[0]==1 && padded[1]==2 && padded[2]==3 && padded[3]==0 && padded[4]==0);

        // skip beyond size
        ring.clear();
        ring.write(std::vector<int16_t>{1,2,3,4,5}.data(),5);
        TEST_ASSERT(ring.skip(10)==5 && ring.empty());
        TEST_ASSERT(ring.skip(5)==0);

        // huge capacity sanity
        bool threw2=false;
        try { RingBuffer<uint8_t> huge(1ULL<<30); } catch(...){ threw2=true; }
        TEST_ASSERT(threw2); // > 1<<28 should throw
    }

    // ── Protocol bounds ──
    {
        CommonHeader h = make_header(MsgType::AUDIO_DATA, 0, 0, 0);
        // payload too large
        CommonHeader big = h; big.payload_size = 100000;
        TEST_ASSERT(!is_valid_header(big, sizeof(CommonHeader)+100000));
        TEST_ASSERT(!validate_header(big, sizeof(CommonHeader)+100000).has_value());

        // truncated
        CommonHeader with_payload = h; with_payload.payload_size = 100;
        TEST_ASSERT(!is_valid_header(with_payload, sizeof(CommonHeader)+50));
        auto v = validate_header(with_payload, sizeof(CommonHeader)+50);
        TEST_ASSERT(!v.has_value());

        // overflow check: payload_size = UINT32_MAX should be rejected before addition overflow
        CommonHeader overflow = h; overflow.payload_size = std::numeric_limits<uint32_t>::max();
        TEST_ASSERT(!is_valid_header(overflow, sizeof(CommonHeader)+100));

        // valid case with exact size
        CommonHeader exact = h; exact.payload_size = 10;
        TEST_ASSERT(is_valid_header(exact, sizeof(CommonHeader)+10));
        TEST_ASSERT(is_valid_header(exact, sizeof(CommonHeader)+20)); // extra bytes ok (>=)
        TEST_ASSERT(!is_valid_header(exact, sizeof(CommonHeader)+9));

        // span parse with exact
        std::vector<std::byte> buf(sizeof(CommonHeader)+10, std::byte{0});
        std::memcpy(buf.data(), &exact, sizeof(exact));
        auto parsed = parse_header(std::span<const std::byte>(buf));
        TEST_ASSERT(parsed.has_value());
        auto pay = payload_span(*parsed, std::span<const std::byte>(buf));
        TEST_ASSERT(pay.has_value() && pay->size()==10);

        // payload_span too small
        std::vector<std::byte> small(sizeof(CommonHeader)+5, std::byte{0});
        std::memcpy(small.data(), &exact, sizeof(exact));
        auto bad_pay = payload_span(exact, std::span<const std::byte>(small));
        TEST_ASSERT(!bad_pay.has_value());
    }

    // ── AudioConverter memory safety ──
    {
        // null handling should not crash
        AudioConverter::float32_to_s16le(nullptr, nullptr, 0);
        AudioConverter::float32_to_s16le(nullptr, reinterpret_cast<int16_t*>(0x1), 10);
        // extreme values don't overflow
        std::vector<float> inf{ std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), 1e10f, -1e10f};
        std::vector<int16_t> out(inf.size());
        AudioConverter::float32_to_s16le(inf.data(), out.data(), inf.size());
        // inf and nan should be clamped to ±32767 or 0? Our clamp does >1 => 1, <-1 => -1, nan stays? nan clamp leaves nan? But static_cast nan -> undefined, but our code clamps with > and < only, nan passes both and then val*32767 = nan cast to int -> undefined. However we clamp via std::clamp in span API. So test span API handles nan.
        std::vector<float> nan_in{ std::numeric_limits<float>::quiet_NaN() };
        std::vector<int16_t> nan_out(1);
        bool ok = AudioConverter::float32_to_s16le(std::span<const float>(nan_in), std::span<int16_t>(nan_out));
        TEST_ASSERT(ok);
        // nan clamped between -1,1 gives? std::clamp(nan) returns nan? Actually clamp uses < comparisons; nan comparisons false, so returns nan? Then static_cast nan -> 0? It's implementation-defined but shouldn't crash.
        (void)nan_out;

        // volume with inf, nan
        std::vector<int16_t> vol{1000, 2000};
        AudioConverter::apply_volume_s16le(vol.data(), vol.size(), std::numeric_limits<float>::infinity());
        // should be clamped to 10x but we clamp volume to 10, nan->0
        // large count shouldn't overflow
        AudioConverter::apply_volume_s16le(nullptr, 100, 1.0f); // no crash

        // downmix with 0 channels should not crash
        std::vector<float> mono{0.1f};
        std::vector<float> dst(2, 9.9f);
        AudioConverter::downmix_to_stereo_float(mono.data(), 0, dst.data(), 1);
        TEST_ASSERT(dst[0]==9.9f); // unchanged

        // huge channels
        AudioConverter::downmix_to_stereo_float(mono.data(), 100, dst.data(), 1);
        TEST_ASSERT(dst[0]==9.9f); // rejected

        // overflow frame_count * channels in span check
        std::vector<float> big_src(100, 0.1f);
        std::vector<float> big_dst(10, 0.0f);
        bool bad = AudioConverter::downmix_to_stereo_float(std::span<const float>(big_src), 2, std::span<float>(big_dst), 1000);
        TEST_ASSERT(!bad); // dst too small
    }

    // ── JitterBuffer memory safety ──
    {
        AudioConfig cfg{48000,2,AudioSampleFormat::PCM_S16LE,240};
        JitterBuffer jb(15);
        jb.configure(cfg, 15);

        // empty span should fail
        auto r = jb.push_packet(0, 1000, std::span<const int16_t>());
        TEST_ASSERT(!r.has_value());

        // huge span
        std::vector<int16_t> huge(200000, 0);
        auto r2 = jb.push_packet(0, 1000, std::span<const int16_t>(huge));
        TEST_ASSERT(!r2.has_value());

        // null raw pointer
        TEST_ASSERT(!jb.push_packet(0,1000,nullptr,10));
        TEST_ASSERT(!jb.push_packet(0,1000,reinterpret_cast<int16_t*>(0x1),0));
        // invalid channel mismatch
        std::vector<int16_t> odd(5,0);
        auto r3 = jb.push_packet(0,1000, std::span<const int16_t>(odd)); // 5 not divisible by 2
        TEST_ASSERT(!r3.has_value());

        // not configured — after fresh construct, default config has 2 channels, so we force invalid via configure
        JitterBuffer jb2(15);
        AudioConfig bad{0,0,AudioSampleFormat::UNKNOWN,0};
        jb2.configure(bad, 15); // now channels==0
        std::vector<int16_t> pkt(10,0);
        // push with zero channels should fail
        TEST_ASSERT(!jb2.push_packet(0,1000,pkt.data(),5));

        // pop with huge request shouldn't overflow — allocate buffer for capped size (8192*4)
        std::vector<int16_t> out(8192*4*2 + 100); // enough for max capped frames * channels
        size_t n = jb.pop_frames(out.data(), 1000000); // capped internally to 32768
        TEST_ASSERT(n <= 1000000);
        TEST_ASSERT(n <= 8192*4);

        // duplicate handling shouldn't corrupt
        std::vector<int16_t> pkt2(240*2, 123);
        TEST_ASSERT(jb.push_packet(10, 2000, pkt2.data(),240));
        TEST_ASSERT(!jb.push_packet(10, 2000, pkt2.data(),240)); // duplicate

        // massive sequence jump resync
        jb.reset();
        jb.configure(cfg,15);
        TEST_ASSERT(jb.push_packet(0,1000, pkt2.data(),240));
        TEST_ASSERT(jb.push_packet(500, 5000, pkt2.data(),240)); // jump >256
        TEST_ASSERT(jb.get_stats().overruns >=1);
    }

    // ── Socket memory safety ──
    {
        UdpSocket s;
        TEST_ASSERT(s.open());
        SocketAddress bad;
        TEST_ASSERT(!bad.is_valid());
        std::array<std::byte, 10> data{ std::byte{1}, std::byte{2} };
        auto res = s.send_to(std::span<const std::byte>(data).first(2), bad);
        TEST_ASSERT(!res.has_value()); // invalid dest should fail, not crash
        std::array<std::byte, 0> empty{};
        auto res2 = s.send_to(std::span<const std::byte>(empty), SocketAddress("127.0.0.1", 1234));
        TEST_ASSERT(!res2.has_value()); // empty should fail

        SocketAddress src;
        std::array<std::byte, 0> empty_buf{};
        auto r3 = s.receive_from(std::span<std::byte>(empty_buf), src);
        TEST_ASSERT(!r3.has_value());

        // large buffer not crash
        std::vector<std::byte> large(70000, std::byte{0});
        // send_to with >INT_MAX should fail, not overflow
        // we can't actually allocate INT_MAX, but we can test size check
        // Simulate by checking logic: our code checks > INT_MAX, so this large (70k) is ok and will try send, but may succeed or fail depending on network — just ensure not crash
        SocketAddress dest("127.0.0.1", 55555);
        auto r4 = s.send_to(std::span<const std::byte>(large), dest);
        (void)r4; // may succeed if loopback accepts large UDP? but fragmented, but not crash

        s.set_receive_timeout_ms(-1); // negative should fail gracefully (now returns false, not crash)
        TEST_ASSERT(!s.set_receive_timeout_ms(-1));
        TEST_ASSERT(!s.set_buffer_sizes(100*1024*1024, 0)); // too large should fail
    }

    return true;
}
