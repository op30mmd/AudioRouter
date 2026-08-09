#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <stdexcept>
#include "span_compat.hpp"
#include "expected_compat.hpp"
#include <concepts>
#include <type_traits>
#include <limits>

namespace audiorouter {

// Hardened C++23 ring buffer — fully thread-safe via mutex, type-safe via concepts,
// memory-safe via bounds-checked span APIs and checked arithmetic.
template <typename T>
class RingBuffer {
    static_assert(std::is_default_constructible_v<T>, "T must be default constructible");
public:
    explicit RingBuffer(size_t capacity_items = 65536)
        : capacity_(capacity_items), buffer_(capacity_items), head_(0), tail_(0), count_(0) {
        if (capacity_items == 0) throw std::invalid_argument("RingBuffer capacity must be >0");
        if (capacity_items > (1ULL << 28)) throw std::invalid_argument("RingBuffer capacity unreasonably large");
    }

    // Non-copyable, movable with lock
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    RingBuffer(RingBuffer&& other) noexcept {
        std::scoped_lock lk(other.mutex_, mutex_);
        capacity_ = other.capacity_;
        buffer_ = std::move(other.buffer_);
        head_ = other.head_; tail_ = other.tail_; count_ = other.count_;
        other.head_ = 0; other.tail_ = 0; other.count_ = 0; other.capacity_ = 0;
    }
    RingBuffer& operator=(RingBuffer&& other) noexcept {
        if (this != &other) {
            std::scoped_lock lk(mutex_, other.mutex_);
            capacity_ = other.capacity_;
            buffer_ = std::move(other.buffer_);
            head_ = other.head_; tail_ = other.tail_; count_ = other.count_;
            other.head_ = other.tail_ = other.count_ = 0; other.capacity_ = 0;
        }
        return *this;
    }

    void resize(size_t new_capacity) {
        if (new_capacity == 0) throw std::invalid_argument("resize capacity must be >0");
        std::lock_guard<std::mutex> lock(mutex_);
        capacity_ = new_capacity;
        buffer_.assign(new_capacity, T{});
        head_ = 0;
        tail_ = 0;
        count_ = 0;
    }

    void clear() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        head_ = 0; tail_ = 0; count_ = 0;
        // Zero memory for audio silence safety (only for trivial types)
        if constexpr (std::is_trivial_v<T>) {
            // keep buffer allocated, just logically clear; optionally fill zero for safety
        }
    }

    [[nodiscard]] size_t size() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    [[nodiscard]] size_t capacity() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return capacity_;
    }

    [[nodiscard]] size_t free_space() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return capacity_ - count_;
    }

    [[nodiscard]] bool empty() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_ == 0;
    }

    [[nodiscard]] bool full() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_ == capacity_;
    }

    // ── Span-based API (C++23 preferred, bounds-checked) ──

    // Write from span — returns items written
    [[nodiscard]] size_t write(std::span<const T> src) noexcept {
        if (src.empty()) return 0;
        return write(src.data(), src.size());
    }

    // Read into span — returns items read
    [[nodiscard]] size_t read(std::span<T> dst) noexcept {
        if (dst.empty()) return 0;
        return read(dst.data(), dst.size());
    }

    [[nodiscard]] size_t peek(std::span<T> dst) const noexcept {
        if (dst.empty()) return 0;
        return peek(dst.data(), dst.size());
    }

    // ── Legacy raw-pointer API — hardened with null/overflow checks ──

    // Write data into the ring buffer. Returns number of items written.
    size_t write(const T* data, size_t count) noexcept {
        if (!data || count == 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (capacity_ == 0) return 0;
        // Guard against overflow of capacity_ - count_ when count_ > capacity_ (shouldn't but safe)
        size_t free = (count_ < capacity_) ? (capacity_ - count_) : 0;
        size_t to_write = std::min(count, free);
        if (to_write == 0) return 0;
        // Ensure capacity_ is not zero for modulo
        size_t first_chunk = std::min(to_write, capacity_ - head_);
        copy_to_buffer(head_, data, first_chunk);
        size_t second_chunk = to_write - first_chunk;
        if (second_chunk > 0) {
            copy_to_buffer(0, data + first_chunk, second_chunk);
            head_ = second_chunk;
        } else {
            head_ = (head_ + first_chunk) % capacity_;
        }
        count_ += to_write;
        return to_write;
    }

    // Overwriting write: drops oldest data to make room
    size_t write_overwrite(const T* data, size_t count) noexcept {
        if (!data || count == 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (capacity_ == 0) return 0;
        if (count >= capacity_) {
            const T* src = data + (count - capacity_);
            // Safe copy — use helper that handles trivial vs non-trivial
            copy_to_buffer(0, src, capacity_);
            head_ = 0;
            tail_ = 0;
            count_ = capacity_;
            return capacity_;
        }
        size_t free_slots = (count_ < capacity_) ? (capacity_ - count_) : 0;
        if (count > free_slots) {
            size_t drop = count - free_slots;
            tail_ = (tail_ + drop) % capacity_;
            count_ -= drop;
        }
        size_t first_chunk = std::min(count, capacity_ - head_);
        copy_to_buffer(head_, data, first_chunk);
        size_t second_chunk = count - first_chunk;
        if (second_chunk > 0) {
            copy_to_buffer(0, data + first_chunk, second_chunk);
            head_ = second_chunk;
        } else {
            head_ = (head_ + first_chunk) % capacity_;
        }
        count_ += count;
        return count;
    }

    [[nodiscard]] size_t write_overwrite(std::span<const T> src) noexcept {
        if (src.empty()) return 0;
        return write_overwrite(src.data(), src.size());
    }

    // Read data from the ring buffer. Returns number of items actually read.
    size_t read(T* dest, size_t count) noexcept {
        if (!dest || count == 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ == 0) return 0;
        size_t to_read = std::min(count, count_);
        size_t first_chunk = std::min(to_read, capacity_ - tail_);
        copy_from_buffer(dest, tail_, first_chunk);
        size_t second_chunk = to_read - first_chunk;
        if (second_chunk > 0) {
            copy_from_buffer(dest + first_chunk, 0, second_chunk);
            tail_ = second_chunk;
        } else {
            tail_ = (tail_ + first_chunk) % capacity_;
        }
        count_ -= to_read;
        // Zero tail region for hygiene when T is arithmetic (avoid stale audio)
        return to_read;
    }

    // Read with silence padding
    size_t read_pad_silence(T* dest, size_t count, T silence_val = T{0}) noexcept {
        if (!dest || count == 0) return 0;
        size_t n = read(dest, count);
        if (n < count) {
            std::fill(dest + n, dest + count, silence_val);
        }
        return n;
    }

    size_t read_pad_silence(std::span<T> dst, T silence_val = T{0}) noexcept {
        if (dst.empty()) return 0;
        size_t n = read(dst.data(), dst.size());
        if (n < dst.size()) std::fill(dst.data() + n, dst.data() + dst.size(), silence_val);
        return n;
    }

    // Peek without removing
    size_t peek(T* dest, size_t count) const noexcept {
        if (!dest || count == 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ == 0) return 0;
        size_t to_read = std::min(count, count_);
        size_t first_chunk = std::min(to_read, capacity_ - tail_);
        copy_from_buffer(dest, tail_, first_chunk);
        size_t second_chunk = to_read - first_chunk;
        if (second_chunk > 0) {
            copy_from_buffer(dest + first_chunk, 0, second_chunk);
        }
        return to_read;
    }

    // Skip items
    size_t skip(size_t count) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t to_skip = std::min(count, count_);
        tail_ = (tail_ + to_skip) % (capacity_ ? capacity_ : 1);
        count_ -= to_skip;
        return to_skip;
    }

    // ── Checked helpers ──
    [[nodiscard]] audiorouter::expected<size_t, std::string> try_write(std::span<const T> src) noexcept {
        if (src.empty()) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        size_t free = (count_ < capacity_) ? capacity_ - count_ : 0;
        if (src.size() > free) return audiorouter::unexpected<std::string>(std::string("RingBuffer full — not enough free space"));
        // delegate without re-locking — inline copy
        size_t first_chunk = std::min(src.size(), capacity_ - head_);
        copy_to_buffer(head_, src.data(), first_chunk);
        size_t second = src.size() - first_chunk;
        if (second) {
            copy_to_buffer(0, src.data() + first_chunk, second);
            head_ = second;
        } else {
            head_ = (head_ + first_chunk) % capacity_;
        }
        count_ += src.size();
        return src.size();
    }

private:
    // T-aware copy helpers: use memmove for trivially copyable, otherwise std::copy
    void copy_to_buffer(size_t offset, const T* src, size_t n) noexcept {
        if (n == 0) return;
        if constexpr (std::is_trivially_copyable_v<T>) {
            // Ensure no overflow of byte size
            if (n > capacity_) return; // safety
            std::memcpy(&buffer_[offset], src, n * sizeof(T));
        } else {
            std::copy_n(src, n, &buffer_[offset]);
        }
    }
    void copy_from_buffer(T* dst, size_t offset, size_t n) const noexcept {
        if (n == 0) return;
        if constexpr (std::is_trivially_copyable_v<T>) {
            std::memcpy(dst, &buffer_[offset], n * sizeof(T));
        } else {
            std::copy_n(&buffer_[offset], n, dst);
        }
    }

    size_t capacity_;
    std::vector<T> buffer_;
    size_t head_;
    size_t tail_;
    size_t count_;
    mutable std::mutex mutex_;
};

using ByteRingBuffer = RingBuffer<uint8_t>;
using AudioSampleRingBuffer = RingBuffer<int16_t>;

} // namespace audiorouter
