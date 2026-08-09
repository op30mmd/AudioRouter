#pragma once

#include <iostream>
#include <sstream>
#include <string>
#include <mutex>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <source_location>

namespace audiorouter {

enum class LogLevel : uint8_t {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

[[nodiscard]] constexpr std::string_view to_string_view(LogLevel lvl) noexcept {
    switch (lvl) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default:              return "UNKNOWN";
    }
}

class Logger {
public:
    static Logger& instance() {
        static Logger logger_instance;
        return logger_instance;
    }

    void set_level(LogLevel level) noexcept {
        current_level_.store(level, std::memory_order_relaxed);
    }

    [[nodiscard]] LogLevel get_level() const noexcept {
        return current_level_.load(std::memory_order_relaxed);
    }

    void set_colored_output(bool enable) noexcept {
        colored_.store(enable, std::memory_order_relaxed);
    }

    [[nodiscard]] bool colored() const noexcept { return colored_.load(std::memory_order_relaxed); }

    void log(LogLevel level, const std::string& message,
             std::source_location loc = std::source_location::current()) {
        if (level < get_level()) return;

        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tm_buf{};
#if defined(_WIN32)
        localtime_s(&tm_buf, &now_time_t);
#else
        localtime_r(&now_time_t, &tm_buf);
#endif
        std::ostringstream time_ss;
        time_ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
                << '.' << std::setfill('0') << std::setw(3) << now_ms.count() << ' ';

        const char* level_str = "";
        const char* color_code = "";
        const char* color_reset = "\033[0m";

        switch (level) {
            case LogLevel::Trace: level_str = "[TRACE]"; color_code = "\033[90m"; break;
            case LogLevel::Debug: level_str = "[DEBUG]"; color_code = "\033[36m"; break;
            case LogLevel::Info:  level_str = "[INFO ]"; color_code = "\033[32m"; break;
            case LogLevel::Warn:  level_str = "[WARN ]"; color_code = "\033[33m"; break;
            case LogLevel::Error: level_str = "[ERROR]"; color_code = "\033[31m"; break;
            case LogLevel::Fatal: level_str = "[FATAL]"; color_code = "\033[35m"; break;
        }

        // Build final line under lock to avoid interleaving; time_str already formatted outside lock
        std::string prefix = time_ss.str();
        std::lock_guard<std::mutex> lock(mutex_);
        // Optionally include source location at Trace/Debug
        std::string loc_suffix;
        if (level == LogLevel::Trace || level == LogLevel::Debug) {
            std::ostringstream ls;
            ls << " (" << loc.file_name() << ':' << loc.line() << ')';
            loc_suffix = ls.str();
        }
        if (colored()) {
            std::cout << color_code << prefix << level_str << ' ' << message << loc_suffix << color_reset << '\n' << std::flush;
        } else {
            std::cout << prefix << level_str << ' ' << message << loc_suffix << '\n' << std::flush;
        }
    }

    // Structured overload taking string_view to avoid temporary allocation when possible
    void log(LogLevel level, std::string_view message,
             std::source_location loc = std::source_location::current()) {
        // forward to string overload to keep single printing path
        log(level, std::string(message), loc);
    }

private:
    Logger() : current_level_(LogLevel::Info), colored_(true) {}
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::atomic<LogLevel> current_level_;
    std::atomic<bool> colored_;
    mutable std::mutex mutex_; // protects cout
};

#define LOG_TRACE(msg) do { std::ostringstream _oss; _oss << msg; audiorouter::Logger::instance().log(audiorouter::LogLevel::Trace, _oss.str()); } while(0)
#define LOG_DEBUG(msg) do { std::ostringstream _oss; _oss << msg; audiorouter::Logger::instance().log(audiorouter::LogLevel::Debug, _oss.str()); } while(0)
#define LOG_INFO(msg)  do { std::ostringstream _oss; _oss << msg; audiorouter::Logger::instance().log(audiorouter::LogLevel::Info , _oss.str()); } while(0)
#define LOG_WARN(msg)  do { std::ostringstream _oss; _oss << msg; audiorouter::Logger::instance().log(audiorouter::LogLevel::Warn , _oss.str()); } while(0)
#define LOG_ERROR(msg) do { std::ostringstream _oss; _oss << msg; audiorouter::Logger::instance().log(audiorouter::LogLevel::Error, _oss.str()); } while(0)
#define LOG_FATAL(msg) do { std::ostringstream _oss; _oss << msg; audiorouter::Logger::instance().log(audiorouter::LogLevel::Fatal, _oss.str()); } while(0)

} // namespace audiorouter
