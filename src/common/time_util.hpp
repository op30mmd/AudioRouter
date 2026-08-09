#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

namespace audiorouter {

[[nodiscard]] inline uint64_t get_time_us() noexcept {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

[[nodiscard]] inline uint64_t get_time_ms() noexcept {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

[[nodiscard]] inline uint64_t get_time_ns() noexcept {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
}

// Monotonic helpers — guarantee non-decreasing (steady_clock is monotonic)
[[nodiscard]] inline std::chrono::steady_clock::time_point steady_now() noexcept {
    return std::chrono::steady_clock::now();
}

inline void sleep_ms(uint32_t ms) noexcept {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline void sleep_us(uint32_t us) noexcept {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

// C++23 chrono helpers
[[nodiscard]] inline double duration_ms(std::chrono::steady_clock::time_point from,
                                        std::chrono::steady_clock::time_point to) noexcept {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

} // namespace audiorouter
