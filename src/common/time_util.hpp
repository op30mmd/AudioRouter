#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

namespace audiorouter {

inline uint64_t get_time_us() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count()
    );
}

inline uint64_t get_time_ms() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()
    );
}

inline uint64_t get_time_ns() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()
    );
}

inline void sleep_ms(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline void sleep_us(uint32_t us) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

} // namespace audiorouter
