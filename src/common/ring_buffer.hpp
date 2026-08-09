#pragma once
// Hardened: thread-safe capacity/full, bounds checks, C++17 compatible

#include <vector>
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <mutex>

namespace audiorouter {

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity_items = 65536)
        : capacity_(capacity_items), buffer_(capacity_items, 0), head_(0), tail_(0), count_(0) {}

    void resize(size_t new_capacity) {
        std::lock_guard<std::mutex> lock(mutex_);
        capacity_ = new_capacity;
        buffer_.assign(new_capacity, 0);
        head_ = 0;
        tail_ = 0;
        count_ = 0;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        head_ = 0;
        tail_ = 0;
        count_ = 0;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    size_t capacity() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return capacity_;
    }

    size_t free_space() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return capacity_ - count_;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_ == 0;
    }

    bool full() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_ == capacity_;
    }

    // Write data into the ring buffer. Returns number of items written.
    size_t write(const T* data, size_t count) {
        if (!data || count == 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);

        size_t to_write = std::min(count, capacity_ - count_);
        if (to_write == 0) return 0;

        size_t first_chunk = std::min(to_write, capacity_ - head_);
        std::memcpy(&buffer_[head_], data, first_chunk * sizeof(T));

        size_t second_chunk = to_write - first_chunk;
        if (second_chunk > 0) {
            std::memcpy(&buffer_[0], data + first_chunk, second_chunk * sizeof(T));
            head_ = second_chunk;
        } else {
            head_ = (head_ + first_chunk) % capacity_;
        }

        count_ += to_write;
        return to_write;
    }

    // Overwriting write: If buffer is full, drops oldest data to make room
    size_t write_overwrite(const T* data, size_t count) {
        if (!data || count == 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);

        if (count >= capacity_) {
            // Write only the last capacity_ items
            const T* src = data + (count - capacity_);
            std::memcpy(&buffer_[0], src, capacity_ * sizeof(T));
            head_ = 0;
            tail_ = 0;
            count_ = capacity_;
            return capacity_;
        }

        size_t free_slots = capacity_ - count_;
        if (count > free_slots) {
            size_t drop = count - free_slots;
            tail_ = (tail_ + drop) % capacity_;
            count_ -= drop;
        }

        size_t first_chunk = std::min(count, capacity_ - head_);
        std::memcpy(&buffer_[head_], data, first_chunk * sizeof(T));

        size_t second_chunk = count - first_chunk;
        if (second_chunk > 0) {
            std::memcpy(&buffer_[0], data + first_chunk, second_chunk * sizeof(T));
            head_ = second_chunk;
        } else {
            head_ = (head_ + first_chunk) % capacity_;
        }

        count_ += count;
        return count;
    }

    // Read data from the ring buffer. Returns number of items actually read.
    size_t read(T* dest, size_t count) {
        if (!dest || count == 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);

        size_t to_read = std::min(count, count_);
        if (to_read == 0) return 0;

        size_t first_chunk = std::min(to_read, capacity_ - tail_);
        std::memcpy(dest, &buffer_[tail_], first_chunk * sizeof(T));

        size_t second_chunk = to_read - first_chunk;
        if (second_chunk > 0) {
            std::memcpy(dest + first_chunk, &buffer_[0], second_chunk * sizeof(T));
            tail_ = second_chunk;
        } else {
            tail_ = (tail_ + first_chunk) % capacity_;
        }

        count_ -= to_read;
        return to_read;
    }

    // Read data with silence padding if not enough data available
    size_t read_pad_silence(T* dest, size_t count, T silence_val = 0) {
        if (!dest || count == 0) return 0;
        size_t read_bytes = read(dest, count);
        if (read_bytes < count) {
            std::fill(dest + read_bytes, dest + count, silence_val);
        }
        return read_bytes;
    }

    // Peek without removing
    size_t peek(T* dest, size_t count) const {
        if (!dest || count == 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);

        size_t to_read = std::min(count, count_);
        if (to_read == 0) return 0;

        size_t first_chunk = std::min(to_read, capacity_ - tail_);
        std::memcpy(dest, &buffer_[tail_], first_chunk * sizeof(T));

        size_t second_chunk = to_read - first_chunk;
        if (second_chunk > 0) {
            std::memcpy(dest + first_chunk, &buffer_[0], second_chunk * sizeof(T));
        }

        return to_read;
    }

    // Skip items
    size_t skip(size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t to_skip = std::min(count, count_);
        tail_ = (tail_ + to_skip) % capacity_;
        count_ -= to_skip;
        return to_skip;
    }

private:
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
