#pragma once

// Compatibility for std::span (C++20) — fallback for C++17
#if __has_include(<span>) && __cplusplus >= 202002L
  #include <span>
#else
  #include <cstddef>
  #include <array>
  #include <type_traits>
  namespace std {
    constexpr size_t dynamic_extent = static_cast<size_t>(-1);
    template <typename T, size_t Extent = dynamic_extent>
    class span {
    public:
      using element_type = T;
      using value_type = std::remove_cv_t<T>;
      using size_type = size_t;
      using pointer = T*;
      using const_pointer = const T*;

      constexpr span() noexcept : data_(nullptr), size_(0) {}
      constexpr span(T* ptr, size_t count) noexcept : data_(ptr), size_(count) {}
      template <size_t N>
      constexpr span(T (&arr)[N]) noexcept : data_(arr), size_(N) {}
      template <typename U, size_t N>
      constexpr span(std::array<U, N>& arr) noexcept : data_(arr.data()), size_(N) {}
      template <typename U, size_t N>
      constexpr span(const std::array<U, N>& arr) noexcept : data_(arr.data()), size_(N) {}
      constexpr span(const span&) noexcept = default;
      span& operator=(const span&) noexcept = default;

      constexpr T* data() const noexcept { return data_; }
      constexpr size_t size() const noexcept { return size_; }
      constexpr bool empty() const noexcept { return size_ == 0; }
      constexpr T& operator[](size_t idx) const noexcept { return data_[idx]; }
      constexpr T* begin() const noexcept { return data_; }
      constexpr T* end() const noexcept { return data_ + size_; }

      constexpr span<T> first(size_t n) const noexcept {
        return span<T>(data_, n < size_ ? n : size_);
      }
      constexpr span<T> last(size_t n) const noexcept {
        return span<T>(data_ + (size_ > n ? size_ - n : 0), n < size_ ? n : size_);
      }
      constexpr span<T> subspan(size_t offset, size_t count = dynamic_extent) const noexcept {
        if (offset > size_) offset = size_;
        size_t remaining = size_ - offset;
        if (count == dynamic_extent || count > remaining) count = remaining;
        return span<T>(data_ + offset, count);
      }

    private:
      T* data_;
      size_t size_;
    };

    // Deduction guides
    template <typename T, size_t N>
    span(T (&)[N]) -> span<T, N>;
    template <typename T, size_t N>
    span(std::array<T, N>&) -> span<T, N>;
    template <typename T, size_t N>
    span(const std::array<T, N>&) -> span<const T, N>;
  } // namespace std
#endif
