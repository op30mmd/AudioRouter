#include "../src/common/ring_buffer.hpp"
#include <iostream>
#include <vector>

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        return false; \
    } \
} while(0)

bool run_ring_buffer_tests() {
    using namespace audiorouter;

    RingBuffer<int16_t> ring(100);
    TEST_ASSERT(ring.capacity() == 100);
    TEST_ASSERT(ring.size() == 0);
    TEST_ASSERT(ring.empty());
    TEST_ASSERT(!ring.full());

    std::vector<int16_t> in_data(40);
    for (int i = 0; i < 40; ++i) in_data[i] = static_cast<int16_t>(i + 1);

    size_t written = ring.write(in_data.data(), in_data.size());
    TEST_ASSERT(written == 40);
    TEST_ASSERT(ring.size() == 40);
    TEST_ASSERT(ring.free_space() == 60);

    std::vector<int16_t> out_data(20);
    size_t read_cnt = ring.read(out_data.data(), 20);
    TEST_ASSERT(read_cnt == 20);
    TEST_ASSERT(ring.size() == 20);
    for (int i = 0; i < 20; ++i) {
        TEST_ASSERT(out_data[i] == static_cast<int16_t>(i + 1));
    }

    // Write another 70 items (total 90)
    std::vector<int16_t> more_data(70);
    for (int i = 0; i < 70; ++i) more_data[i] = static_cast<int16_t>(100 + i);
    written = ring.write(more_data.data(), more_data.size());
    TEST_ASSERT(written == 70);
    TEST_ASSERT(ring.size() == 90);

    // Read remaining
    std::vector<int16_t> all_read(90);
    read_cnt = ring.read(all_read.data(), 90);
    TEST_ASSERT(read_cnt == 90);
    TEST_ASSERT(ring.empty());

    // Check first 20 items (were 21..40 from first write)
    for (int i = 0; i < 20; ++i) {
        TEST_ASSERT(all_read[i] == static_cast<int16_t>(21 + i));
    }
    // Next 70 items (were 100..169)
    for (int i = 0; i < 70; ++i) {
        TEST_ASSERT(all_read[20 + i] == static_cast<int16_t>(100 + i));
    }

    // Test overwrite
    ring.clear();
    std::vector<int16_t> big_data(120);
    for (int i = 0; i < 120; ++i) big_data[i] = static_cast<int16_t>(i);
    ring.write_overwrite(big_data.data(), big_data.size());
    TEST_ASSERT(ring.full());
    TEST_ASSERT(ring.size() == 100);

    std::vector<int16_t> read_big(100);
    ring.read(read_big.data(), 100);
    // Should contain last 100 items (20..119)
    TEST_ASSERT(read_big[0] == 20);
    TEST_ASSERT(read_big[99] == 119);

    return true;
}
