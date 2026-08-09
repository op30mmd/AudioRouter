#pragma once

// Compatibility layer for std::expected (C++23) — falls back to a minimal
// implementation when <expected> is not available (e.g., GCC 11 / C++17).
#if __has_include(<expected>)
  #include <expected>
#endif
#if defined(__cpp_lib_expected)
  namespace audiorouter {
    template <typename T, typename E>
    using expected = std::expected<T, E>;
    using std::unexpected;
  }
#else
  // Minimal polyfill for the subset of std::expected used in this project.
  // Supports expected<T,E> and expected<void,E> with has_value(), value(), error(), and unexpected().
  #include <variant>
  #include <string>
  #include <utility>
  #include <type_traits>

  namespace audiorouter {

  template <typename E>
  struct unexpected {
      E err;
      explicit unexpected(const E& e) : err(e) {}
      explicit unexpected(E&& e) : err(std::move(e)) {}
      const E& error() const noexcept { return err; }
  };

  template <typename T, typename E>
  class expected {
  public:
      // value constructors
      expected(const T& v) : storage_(v), has_val_(true) {}
      expected(T&& v) : storage_(std::move(v)), has_val_(true) {}
      expected(const unexpected<E>& u) : storage_(u.err), has_val_(false) {}
      expected(unexpected<E>&& u) : storage_(std::move(u.err)), has_val_(false) {}

      bool has_value() const noexcept { return has_val_; }
      explicit operator bool() const noexcept { return has_val_; }

      T& value() & { return std::get<T>(storage_); }
      const T& value() const & { return std::get<T>(storage_); }
      T&& value() && { return std::get<T>(std::move(storage_)); }
      const T&& value() const && { return std::get<T>(std::move(storage_)); }

      E& error() & { return std::get<E>(storage_); }
      const E& error() const & { return std::get<E>(storage_); }
      const E&& error() const && { return std::get<E>(std::move(storage_)); }

      T& operator*() & { return value(); }
      const T& operator*() const & { return value(); }
      T&& operator*() && { return std::move(value()); }
      const T&& operator*() const && { return std::move(value()); }
      T* operator->() { return &value(); }
      const T* operator->() const { return &value(); }

  private:
      std::variant<T, E> storage_;
      bool has_val_;
  };

  // Specialization for void
  template <typename E>
  class expected<void, E> {
  public:
      expected() : has_val_(true) {}
      expected(const unexpected<E>& u) : storage_(u.err), has_val_(false) {}
      expected(unexpected<E>&& u) : storage_(std::move(u.err)), has_val_(false) {}

      bool has_value() const noexcept { return has_val_; }
      explicit operator bool() const noexcept { return has_val_; }

      void value() const noexcept {}

      E& error() & { return std::get<E>(storage_); }
      const E& error() const & { return std::get<E>(storage_); }

  private:
      std::variant<std::monostate, E> storage_; // monostate for void success
      bool has_val_;
      // For void, we store E in variant when error, monostate when ok
  };

  } // namespace audiorouter
#endif
