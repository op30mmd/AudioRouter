#include "../src/common/audio_types.hpp"
#include "../src/common/protocol.hpp"
#include "../src/common/socket_util.hpp"
#include "../src/common/ring_buffer.hpp"
#include <iostream>
#include "../src/common/span_compat.hpp"
#include "../src/common/expected_compat.hpp"

#define TEST_ASSERT(cond) do { if (!(cond)) { std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; return false; } } while(0)

bool run_type_safety_tests() {
    using namespace audiorouter;
    using namespace audiorouter::protocol;

    // ── AudioConfig validation & strong typing ──
    {
        AudioConfig ok{48000, 2, AudioSampleFormat::PCM_S16LE, 240};
        TEST_ASSERT(ok.is_valid());
        TEST_ASSERT(ok.validate().has_value());
        TEST_ASSERT(ok.bytes_per_sample() == 2);
        TEST_ASSERT(ok.bytes_per_frame() == 4);
        TEST_ASSERT(ok.packet_payload_size() == 960);

        AudioConfig bad_rate{0, 2, AudioSampleFormat::PCM_S16LE, 240};
        TEST_ASSERT(!bad_rate.is_valid());
        TEST_ASSERT(!bad_rate.validate().has_value());

        AudioConfig bad_ch{48000, 0, AudioSampleFormat::PCM_S16LE, 240};
        TEST_ASSERT(!bad_ch.is_valid());

        AudioConfig bad_fmt{48000, 2, AudioSampleFormat::UNKNOWN, 240};
        TEST_ASSERT(!bad_fmt.is_valid());

        AudioConfig huge_frames{48000, 2, AudioSampleFormat::PCM_S16LE, 100000};
        TEST_ASSERT(!huge_frames.is_valid());

        // Equality
        AudioConfig a = ok, b = ok;
        TEST_ASSERT(a == b);
        b.channels = 1;
        TEST_ASSERT(a != b);

        // to_string_view
        TEST_ASSERT(to_string_view(AudioSampleFormat::PCM_S16LE) == "S16LE");
        TEST_ASSERT(to_string_view(AudioSampleFormat::PCM_FLOAT32LE) == "FLOAT32LE");
        TEST_ASSERT(to_string_view(AudioSampleFormat::UNKNOWN) == "UNKNOWN");
    }

    // ── Protocol type safety ──
    {
        TEST_ASSERT(is_valid_msg_type(static_cast<uint8_t>(MsgType::AUDIO_DATA)));
        TEST_ASSERT(!is_valid_msg_type(0xFF));
        TEST_ASSERT(msg_type_to_string(MsgType::CONNECT_ACK) == "CONNECT_ACK");
        TEST_ASSERT(msg_type_to_string(MsgType::UNKNOWN) == "UNKNOWN");

        CommonHeader h = make_header(MsgType::AUDIO_DATA, 1, 1000, 0);
        TEST_ASSERT(h.magic == MAGIC);
        TEST_ASSERT(h.version == CURRENT_VERSION);
        TEST_ASSERT(is_valid_header(h, sizeof(CommonHeader)));
        // payload size overflow should be invalid
        CommonHeader bad = h; bad.payload_size = 70000;
        TEST_ASSERT(!is_valid_header(bad, sizeof(CommonHeader)+70000));
        // truncated
        TEST_ASSERT(!is_valid_header(h, 10));
        // bad magic
        bad = h; bad.magic = 0xDEADBEEF; TEST_ASSERT(!is_valid_header(bad, sizeof(CommonHeader)));
        // bad version
        bad = h; bad.version = 99; TEST_ASSERT(!is_valid_header(bad, sizeof(CommonHeader)));
        // flags unknown
        bad = h; bad.flags = 0xFFFF; TEST_ASSERT(!is_valid_header(bad, sizeof(CommonHeader)));

        // span-based parse
        std::array<std::byte, sizeof(CommonHeader)> buf{};
        std::memcpy(buf.data(), &h, sizeof(h));
        auto parsed = parse_header(std::span<const std::byte>(buf));
        TEST_ASSERT(parsed.has_value());
        TEST_ASSERT(parsed->seq_num == 1);

        std::array<std::byte, 5> small{}; // too small
        auto bad_parse = parse_header(small);
        TEST_ASSERT(!bad_parse.has_value());

        // Audio header validation
        AudioPacketHeader ah{}; ah.common = h; ah.common.msg_type = static_cast<uint8_t>(MsgType::AUDIO_DATA);
        ah.sample_rate = 48000; ah.channels = 2; ah.format = static_cast<uint8_t>(AudioSampleFormat::PCM_S16LE);
        ah.num_frames = 240; ah.common.payload_size = (sizeof(AudioPacketHeader)-sizeof(CommonHeader)) + 240*2*2;
        // need to pass received_bytes = header + payload
        size_t total = sizeof(AudioPacketHeader) + 240*2*2;
        TEST_ASSERT(validate_audio_header(ah, total).has_value());
        ah.num_frames = 0; TEST_ASSERT(!validate_audio_header(ah, total).has_value());
    }

    // ── SocketAddress strong typing ──
    {
        auto ok = SocketAddress::create("127.0.0.1", 44100);
        TEST_ASSERT(ok.has_value());
        TEST_ASSERT(ok->is_valid());
        TEST_ASSERT(ok->ip() == "127.0.0.1");
        TEST_ASSERT(ok->port() == 44100);

        auto bad = SocketAddress::create("999.999.999.999", 80);
        TEST_ASSERT(!bad.has_value());

        SocketAddress a("192.168.1.1", 1234), b("192.168.1.1", 1234), c("10.0.0.1", 1234);
        TEST_ASSERT(a == b);
        TEST_ASSERT(a != c);
        TEST_ASSERT(!(a == SocketAddress()));

        SocketAddress any("0.0.0.0", 0);
        TEST_ASSERT(any.is_valid());
        TEST_ASSERT(any.ip() == "0.0.0.0");
    }

    // ── AudioConverter span type safety ──
    {
        std::vector<float> f{0.0f, 1.0f, -1.0f, 0.5f};
        std::vector<int16_t> s(f.size()), back(f.size());
        std::vector<float> fout(f.size());
        TEST_ASSERT(AudioConverter::float32_to_s16le(std::span<const float>(f), std::span<int16_t>(s)));
        TEST_ASSERT(s[0]==0 && s[1]==32767 && s[2]==-32767);
        TEST_ASSERT(AudioConverter::s16le_to_float32(std::span<const int16_t>(s), std::span<float>(fout)));
        // span size mismatch should fail
        std::vector<int16_t> small(2);
        TEST_ASSERT(!AudioConverter::float32_to_s16le(std::span<const float>(f), std::span<int16_t>(small)));

        // volume: span overload clamps
        std::vector<int16_t> vol{1000, -1000};
        AudioConverter::apply_volume_s16le(std::span<int16_t>(vol), 0.5f);
        TEST_ASSERT(vol[0]==500 && vol[1]==-500);
        // nan volume should zero
        std::vector<int16_t> nanvol{100, 200};
        AudioConverter::apply_volume_s16le(std::span<int16_t>(nanvol), std::numeric_limits<float>::quiet_NaN());
        TEST_ASSERT(nanvol[0]==0 && nanvol[1]==0);

        // downmix span API
        std::vector<float> mono{0.5f, -0.5f};
        std::vector<float> stereo(4);
        TEST_ASSERT(AudioConverter::downmix_to_stereo_float(std::span<const float>(mono), 1, std::span<float>(stereo), 2));
        TEST_ASSERT(stereo[0]==0.5f && stereo[1]==0.5f);
        TEST_ASSERT(!AudioConverter::downmix_to_stereo_float(std::span<const float>(mono), 1, std::span<float>(stereo).first(2), 2)); // dst too small
    }

    // ── RingBuffer generic type safety ──
    {
        RingBuffer<float> rf(10);
        TEST_ASSERT(rf.capacity() == 10);
        std::vector<float> in{1.0f, 2.0f, 3.0f};
        TEST_ASSERT(rf.write(std::span<const float>(in)) == 3);
        std::vector<float> out(3);
        TEST_ASSERT(rf.read(std::span<float>(out)) == 3);
        TEST_ASSERT(out[0]==1.0f);

        // try_write fails when full
        RingBuffer<int> small(2);
        TEST_ASSERT(small.write(std::span<const int>(std::vector<int>{1,2}))==2);
        auto res = small.try_write(std::span<const int>(std::vector<int>{3}));
        TEST_ASSERT(!res.has_value());

        // non-trivial type? std::string is not trivially copyable but should still work via copy_n fallback
        RingBuffer<std::string> rs(3);
        std::vector<std::string> s_in{"a","b"};
        TEST_ASSERT(rs.write(s_in.data(), s_in.size())==2);
        std::vector<std::string> s_out(2);
        TEST_ASSERT(rs.read(s_out.data(),2)==2);
        TEST_ASSERT(s_out[0]=="a");
    }

    // ── std::expected usage ──
    {
        std::expected<int, std::string> e = 42;
        TEST_ASSERT(e.has_value() && e.value()==42);
        std::expected<int, std::string> err = std::unexpected<std::string>(std::string("oops"));
        TEST_ASSERT(!err.has_value() && err.error()=="oops");
    }

    return true;
}
