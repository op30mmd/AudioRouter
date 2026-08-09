#include "../src/common/protocol.hpp"
#include "../src/common/audio_types.hpp"
#include <iostream>
#include <cassert>
#include <cstring>

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        return false; \
    } \
} while(0)

bool run_protocol_tests() {
    using namespace audiorouter;
    using namespace audiorouter::protocol;

    // Check packed struct sizes to ensure cross-platform compatibility
    TEST_ASSERT(sizeof(CommonHeader) == (4 + 1 + 1 + 2 + 4 + 8 + 4)); // 24 bytes
    TEST_ASSERT(sizeof(AudioPacketHeader) == (24 + 4 + 2 + 1 + 1 + 4)); // 36 bytes

    // Check header validation
    CommonHeader hdr;
    hdr.magic = MAGIC;
    hdr.version = CURRENT_VERSION;
    hdr.msg_type = static_cast<uint8_t>(MsgType::AUDIO_DATA);
    hdr.flags = FLAG_NONE;
    hdr.seq_num = 42;
    hdr.timestamp_us = 1000000;
    hdr.payload_size = 960;

    TEST_ASSERT(is_valid_header(hdr, sizeof(CommonHeader) + 960));
    TEST_ASSERT(!is_valid_header(hdr, sizeof(CommonHeader) + 500)); // Too short

    CommonHeader bad_magic = hdr;
    bad_magic.magic = 0x12345678;
    TEST_ASSERT(!is_valid_header(bad_magic, sizeof(CommonHeader) + 960));

    CommonHeader bad_ver = hdr;
    bad_ver.version = 99;
    TEST_ASSERT(!is_valid_header(bad_ver, sizeof(CommonHeader) + 960));

    // Check AudioConfig calculations
    AudioConfig config;
    config.sample_rate = 48000;
    config.channels = 2;
    config.format = AudioSampleFormat::PCM_S16LE;
    config.frames_per_packet = 240;

    TEST_ASSERT(config.bytes_per_sample() == 2);
    TEST_ASSERT(config.bytes_per_frame() == 4);
    TEST_ASSERT(config.packet_payload_size() == 960);
    TEST_ASSERT(config.frames_to_bytes(240) == 960);
    TEST_ASSERT(config.bytes_to_frames(960) == 240);
    TEST_ASSERT(config.packet_duration_ms() == 5.0);

    return true;
}
