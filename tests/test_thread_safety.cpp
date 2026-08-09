#include "../src/common/ring_buffer.hpp"
#include "../src/common/logger.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        return false; \
    } \
} while(0)

bool test_ring_buffer_concurrent() {
    using namespace audiorouter;

    constexpr size_t BUFFER_CAPACITY = 2000;
    RingBuffer<int> ring(BUFFER_CAPACITY);
    std::atomic<bool> done{false};
    std::atomic<size_t> total_written{0};
    std::atomic<size_t> total_read{0};
    std::atomic<bool> reader_error{false};

    // Single writer thread writing sequential integers
    std::thread writer([&]() {
        int val = 0;
        while (val < 10000) {
            // Write one by one or in small chunks
            size_t n = ring.write(&val, 1);
            if (n > 0) {
                val++;
                total_written++;
            } else {
                std::this_thread::yield();
            }
        }
        done = true;
    });

    // Single reader thread reading and checking order
    std::thread reader([&]() {
        int expected_val = 0;
        while (!done || !ring.empty()) {
            int val = 0;
            size_t n = ring.read(&val, 1);
            if (n > 0) {
                if (val != expected_val) {
                    std::cerr << "Mismatched value: expected " << expected_val << ", got " << val << "\n";
                    reader_error = true;
                    break;
                }
                expected_val++;
                total_read++;
            } else {
                std::this_thread::yield();
            }
        }
    });

    writer.join();
    reader.join();

    TEST_ASSERT(!reader_error);
    TEST_ASSERT(total_written == 10000);
    TEST_ASSERT(total_read == 10000);

    return true;
}

bool test_logger_concurrent() {
    // Spawn multiple threads and log concurrently
    std::vector<std::thread> threads;
    std::atomic<int> count{0};

    for (int i = 0; i < 5; ++i) {
        threads.emplace_back([&, i]() {
            for (int j = 0; j < 50; ++j) {
                LOG_DEBUG("Thread " << i << " logging message " << j);
                LOG_INFO("Thread " << i << " logging info " << j);
                count++;
            }
        });
    }

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    TEST_ASSERT(count == 250);
    return true;
}

bool run_thread_safety_tests() {
    TEST_ASSERT(test_ring_buffer_concurrent());
    TEST_ASSERT(test_logger_concurrent());
    return true;
}
