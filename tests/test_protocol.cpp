#include "../src/common/protocol.hpp"
#include "../src/common/audio_types.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include "../src/common/span_compat.hpp"
#include <array>
#include <vector>

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        return false; \
    } \
} while(0)

bool run_protocol_tests() {
    using namespace audiorouter;
    using namespace audiorouter::protocol;

    // Check packed struct sizes
    TEST_ASSERT(sizeof(CommonHeader) == (4 + 1 + 1 + 2 + 4 + 8 + 4)); // 24 bytes
    TEST_ASSERT(sizeof(AudioPacketHeader) == (24 + 4 + 2 + 1 + 1 + 4)); // 36 bytes
    static_assert(sizeof(CommonHeader)==24);
    static_assert(sizeof(AudioPacketHeader)==36);

    // Header validation exhaustive
    CommonHeader hdr{};
    hdr.magic = MAGIC;
    hdr.version = CURRENT_VERSION;
    hdr.msg_type = static_cast<uint8_t>(MsgType::AUDIO_DATA);
    hdr.flags = FLAG_NONE;
    hdr.seq_num = 42;
    hdr.timestamp_us = 1000000;
    hdr.payload_size = 960;

    TEST_ASSERT(is_valid_header(hdr, sizeof(CommonHeader) + 960));
    TEST_ASSERT(validate_header(hdr, sizeof(CommonHeader)+960).has_value());
    TEST_ASSERT(!is_valid_header(hdr, sizeof(CommonHeader) + 500));
    TEST_ASSERT(!validate_header(hdr, sizeof(CommonHeader)+500).has_value());

    CommonHeader bad_magic = hdr; bad_magic.magic = 0x12345678;
    TEST_ASSERT(!is_valid_header(bad_magic, sizeof(CommonHeader) + 960));

    CommonHeader bad_ver = hdr; bad_ver.version = 99;
    TEST_ASSERT(!is_valid_header(bad_ver, sizeof(CommonHeader) + 960));

    // invalid msg_type
    CommonHeader bad_type = hdr; bad_type.msg_type = 0xFF;
    TEST_ASSERT(!is_valid_header(bad_type, sizeof(CommonHeader)+960));
    TEST_ASSERT(!is_valid_msg_type(0xFF));
    TEST_ASSERT(is_valid_msg_type(static_cast<uint8_t>(MsgType::CONNECT_ACK)));

    // flags unknown should fail
    CommonHeader bad_flags = hdr; bad_flags.flags = 0xFF00;
    TEST_ASSERT(!is_valid_header(bad_flags, sizeof(CommonHeader)+960));

    // payload too large
    CommonHeader huge = hdr; huge.payload_size = 70000;
    TEST_ASSERT(!is_valid_header(huge, sizeof(CommonHeader)+70000));

    // msg_type string coverage (C++23 string_view)
    TEST_ASSERT(msg_type_to_string(MsgType::DISCOVERY_REQ) == "DISCOVERY_REQ");
    TEST_ASSERT(msg_type_to_string(MsgType::DISCOVERY_RESP) == "DISCOVERY_RESP");
    TEST_ASSERT(msg_type_to_string(MsgType::CONNECT_REQ) == "CONNECT_REQ");
    TEST_ASSERT(msg_type_to_string(MsgType::AUDIO_DATA) == "AUDIO_DATA");
    TEST_ASSERT(msg_type_to_string(MsgType::HEARTBEAT_PING) == "HEARTBEAT_PING");
    TEST_ASSERT(msg_type_to_string(MsgType::HEARTBEAT_PONG) == "HEARTBEAT_PONG");
    TEST_ASSERT(msg_type_to_string(MsgType::CONTROL_CMD) == "CONTROL_CMD");
    TEST_ASSERT(msg_type_to_string(static_cast<MsgType>(99)) == "UNKNOWN");

    // make_header factory
    auto mk = make_header(MsgType::CONNECT_REQ, 123, 456789, 32);
    TEST_ASSERT(mk.magic == MAGIC && mk.seq_num==123 && mk.payload_size==32);
    TEST_ASSERT(is_valid_header(mk, sizeof(CommonHeader)+32));

    // span-based parse — use header with zero payload for simple header-only buffer
    {
        CommonHeader hdr0 = make_header(MsgType::HEARTBEAT_PING, 42, 9999, 0);
        std::array<std::byte, sizeof(CommonHeader)> buf{};
        std::memcpy(buf.data(), &hdr0, sizeof(hdr0));
        auto parsed = parse_header(std::span<const std::byte>(buf));
        TEST_ASSERT(parsed.has_value() && parsed->seq_num==42);
        auto bad_span = parse_header(std::span<const std::byte>(buf).first(10));
        TEST_ASSERT(!bad_span.has_value());
        // also test with payload case: full buffer includes payload
        std::vector<std::byte> withPayload(sizeof(CommonHeader)+32, std::byte{0});
        CommonHeader hp = make_header(MsgType::CONNECT_REQ, 7, 12345, 32);
        std::memcpy(withPayload.data(), &hp, sizeof(hp));
        auto parsed2 = parse_header(std::span<const std::byte>(withPayload));
        TEST_ASSERT(parsed2.has_value() && parsed2->seq_num==7);
    }

    // payload_span
    std::vector<std::byte> full(sizeof(CommonHeader)+960, std::byte{0});
    std::memcpy(full.data(), &hdr, sizeof(hdr));
    auto pay = payload_span(hdr, std::span<const std::byte>(full));
    TEST_ASSERT(pay.has_value() && pay->size()==960);
    std::vector<std::byte> truncated(sizeof(CommonHeader)+10, std::byte{0});
    std::memcpy(truncated.data(), &hdr, sizeof(hdr));
    auto bad_pay = payload_span(hdr, std::span<const std::byte>(truncated));
    TEST_ASSERT(!bad_pay.has_value());

    // Audio header validation
    AudioPacketHeader ah{};
    ah.common = make_header(MsgType::AUDIO_DATA, 1, 1000, 0);
    ah.sample_rate = 48000; ah.channels = 2; ah.format = static_cast<uint8_t>(AudioSampleFormat::PCM_S16LE);
    ah.num_frames = 240;
    size_t pcm_bytes = 240*2*2;
    ah.common.payload_size = static_cast<uint32_t>((sizeof(AudioPacketHeader)-sizeof(CommonHeader))+pcm_bytes);
    size_t total = sizeof(AudioPacketHeader)+pcm_bytes;
    TEST_ASSERT(validate_audio_header(ah, total).has_value());
    // invalid sample rate
    ah.sample_rate = 0; TEST_ASSERT(!validate_audio_header(ah, total).has_value()); ah.sample_rate=48000;
    // invalid channels
    ah.channels = 0; TEST_ASSERT(!validate_audio_header(ah, total).has_value()); ah.channels=2;
    // invalid frames
    ah.num_frames = 0; TEST_ASSERT(!validate_audio_header(ah, total).has_value()); ah.num_frames=240;
    // truncated
    TEST_ASSERT(!validate_audio_header(ah, total-10).has_value());

    // AudioConfig calculations + validation (broad)
    AudioConfig config{};
    config.sample_rate = 48000; config.channels = 2; config.format = AudioSampleFormat::PCM_S16LE; config.frames_per_packet = 240;
    TEST_ASSERT(config.bytes_per_sample() == 2);
    TEST_ASSERT(config.bytes_per_frame() == 4);
    TEST_ASSERT(config.packet_payload_size() == 960);
    TEST_ASSERT(config.frames_to_bytes(240) == 960);
    TEST_ASSERT(config.bytes_to_frames(960) == 240);
    TEST_ASSERT(config.packet_duration_ms() == 5.0);
    TEST_ASSERT(config.is_valid());
    TEST_ASSERT(config.validate().has_value());
    TEST_ASSERT(config.to_string().find("48000Hz") != std::string::npos);

    // variant formats
    for (auto fmt : {AudioSampleFormat::PCM_S16LE, AudioSampleFormat::PCM_FLOAT32LE, AudioSampleFormat::PCM_S24LE, AudioSampleFormat::PCM_S32LE}) {
        config.format = fmt;
        TEST_ASSERT(config.bytes_per_sample() >0);
        TEST_ASSERT(config.is_valid());
    }
    // edge: mono, high rate
    config = {44100,1,AudioSampleFormat::PCM_S16LE, 512};
    TEST_ASSERT(config.bytes_per_frame()==2);
    TEST_ASSERT(config.packet_payload_size()==1024);
    TEST_ASSERT(config.is_valid());
    // invalid
    config.sample_rate = 0; TEST_ASSERT(!config.is_valid());
    config.sample_rate = 48000; config.channels=0; TEST_ASSERT(!config.is_valid());
    config.channels=2; config.format=AudioSampleFormat::UNKNOWN; TEST_ASSERT(!config.is_valid());

    // Overflow guards
    AudioConfig big{48000, 32, AudioSampleFormat::PCM_S32LE, 8192};
    [[maybe_unused]] size_t payload = big.packet_payload_size(); (void)payload; // 8192*32*4 = 1,048,576 > max? but is_valid should reject >65507
    TEST_ASSERT(!big.is_valid());

    return true;
}
