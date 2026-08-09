#include "../src/common/protocol.hpp"
#include "../src/common/ring_buffer.hpp"
#include "../src/common/audio_types.hpp"
#include <iostream>
#include <vector>
#include <cstring>

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        return false; \
    } \
} while(0)

bool test_ring_buffer_bounds_and_hygiene() {
    using namespace audiorouter;

    RingBuffer<int> ring(10);

    // Write some data, read some data
    std::vector<int> write_data = {1, 2, 3, 4, 5};
    size_t w = ring.write(write_data);
    TEST_ASSERT(w == 5);

    std::vector<int> read_data(3);
    size_t r = ring.read(read_data);
    TEST_ASSERT(r == 3);
    TEST_ASSERT(read_data[0] == 1);
    TEST_ASSERT(read_data[1] == 2);
    TEST_ASSERT(read_data[2] == 3);

    // Zero / NULL pointer safety on legacy API
    size_t null_w = ring.write(nullptr, 100);
    TEST_ASSERT(null_w == 0);

    size_t null_r = ring.read(nullptr, 100);
    TEST_ASSERT(null_r == 0);

    // Overwrite safety: if we write more than capacity, it should clamp to capacity and write only the trailing elements
    std::vector<int> big_write = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
    size_t w_over = ring.write_overwrite(big_write);
    TEST_ASSERT(w_over == 10); // buffer capacity is 10

    std::vector<int> read_big(10);
    size_t r_big = ring.read(read_big);
    TEST_ASSERT(r_big == 10);
    TEST_ASSERT(read_big[0] == 30); // 10, 20 are dropped
    TEST_ASSERT(read_big[9] == 120);

    // Empty buffer check after full read
    TEST_ASSERT(ring.empty());

    return true;
}

bool test_protocol_overflow_prevention() {
    using namespace audiorouter::protocol;

    // Construct a CommonHeader with a payload size that claims to be very large
    CommonHeader hdr;
    hdr.magic = MAGIC;
    hdr.version = CURRENT_VERSION;
    hdr.msg_type = static_cast<uint8_t>(MsgType::AUDIO_DATA);
    hdr.flags = FLAG_NONE;
    hdr.seq_num = 1;
    hdr.timestamp_us = 1000;
    hdr.payload_size = 50000; // Claims 50000 bytes

    // If we only received 100 bytes, validate_header must immediately reject this to prevent out-of-bounds reads / heap overflow
    size_t received_bytes = 100;
    auto res = validate_header(hdr, received_bytes);
    TEST_ASSERT(!res.has_value());
    TEST_ASSERT(res.error() == "truncated payload" || res.error() == "payload too large");

    // Extremely large payload size exceeding MAX_UDP_PACKET_SIZE should be rejected immediately
    hdr.payload_size = 9999999;
    res = validate_header(hdr, 10000000);
    TEST_ASSERT(!res.has_value());
    TEST_ASSERT(res.error() == "payload too large");

    // Invalid message type
    hdr.payload_size = 10;
    hdr.msg_type = 0x99; // Invalid type
    res = validate_header(hdr, sizeof(CommonHeader) + 10);
    TEST_ASSERT(!res.has_value());
    TEST_ASSERT(res.error() == "unknown msg_type");

    return true;
}

bool test_audio_header_bounds() {
    using namespace audiorouter::protocol;

    AudioPacketHeader ah;
    ah.common.magic = MAGIC;
    ah.common.version = CURRENT_VERSION;
    ah.common.msg_type = static_cast<uint8_t>(MsgType::AUDIO_DATA);
    ah.common.flags = FLAG_NONE;
    ah.common.seq_num = 1;
    ah.common.timestamp_us = 1000;
    ah.format = static_cast<uint8_t>(audiorouter::AudioSampleFormat::PCM_S16LE);

    // Case 1: Unreasonable sample rate
    ah.sample_rate = 999999;
    ah.channels = 2;
    ah.num_frames = 240;
    ah.common.payload_size = (sizeof(AudioPacketHeader) - sizeof(CommonHeader)) + (240 * 2 * 2);
    auto res = validate_audio_header(ah, sizeof(AudioPacketHeader) + (240 * 2 * 2));
    TEST_ASSERT(!res.has_value());
    TEST_ASSERT(res.error() == "invalid sample_rate");

    // Case 2: Unreasonable channel count (e.g. 100 channels)
    ah.sample_rate = 48000;
    ah.channels = 100;
    ah.num_frames = 240;
    ah.common.payload_size = (sizeof(AudioPacketHeader) - sizeof(CommonHeader)) + (240 * 100 * 2);
    res = validate_audio_header(ah, sizeof(AudioPacketHeader) + (240 * 100 * 2));
    TEST_ASSERT(!res.has_value());
    TEST_ASSERT(res.error() == "invalid channels");

    // Case 3: Too many frames per packet (e.g. 9000 frames)
    ah.sample_rate = 48000;
    ah.channels = 2;
    ah.num_frames = 9000;
    ah.common.payload_size = (sizeof(AudioPacketHeader) - sizeof(CommonHeader)) + (9000 * 2 * 2);
    res = validate_audio_header(ah, sizeof(AudioPacketHeader) + (9000 * 2 * 2));
    TEST_ASSERT(!res.has_value());
    TEST_ASSERT(res.error() == "invalid num_frames");

    return true;
}

bool run_memory_safety_tests() {
    TEST_ASSERT(test_ring_buffer_bounds_and_hygiene());
    TEST_ASSERT(test_protocol_overflow_prevention());
    TEST_ASSERT(test_audio_header_bounds());
    return true;
}
