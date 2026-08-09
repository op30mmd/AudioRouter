#pragma once

// Compatibility for std::jthread / std::stop_token (C++20) — fallback to std::thread
#if __has_include(<stop_token>) && __has_include(<thread>) && defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
  #include <stop_token>
  #include <thread>
  namespace audiorouter {
    using jthread = std::jthread;
    using stop_token = std::stop_token;
  }
  #define AUDIOROUTER_HAS_JTHREAD 1
#else
  #include <thread>
  #include <atomic>
  namespace audiorouter {
    struct stop_token {
      bool stop_requested() const noexcept { return false; }
    };
    class jthread : public std::thread {
    public:
      jthread() noexcept = default;
      template <typename Callable, typename... Args>
      explicit jthread(Callable&& f, Args&&... args)
        : std::thread(std::forward<Callable>(f), stop_token{}, std::forward<Args>(args)...) {}
      // std::thread already has joinable() and join()
      void request_stop() noexcept {}
    };
  }
  #define AUDIOROUTER_HAS_JTHREAD 0
#endif
