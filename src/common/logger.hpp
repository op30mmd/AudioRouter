#pragma once

#include <iostream>
#include <sstream>
#include <string>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <ctime>

namespace audiorouter {

enum class LogLevel {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

class Logger {
public:
    static Logger& instance() {
        static Logger logger_instance;
        return logger_instance;
    }

    void set_level(LogLevel level) {
        std::lock_guard<std::mutex> lock(mutex_);
        current_level_ = level;
    }

    LogLevel get_level() const {
        return current_level_;
    }

    void set_colored_output(bool enable) {
        std::lock_guard<std::mutex> lock(mutex_);
        colored_ = enable;
    }

    void log(LogLevel level, const std::string& message) {
        if (level < current_level_) {
            return;
        }

        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch()) % 1000;

        std::tm tm_buf;
#if defined(_WIN32)
        localtime_s(&tm_buf, &now_time_t);
#else
        localtime_r(&now_time_t, &tm_buf);
#endif

        std::stringstream ss;
        ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
           << '.' << std::setfill('0') << std::setw(3) << now_ms.count() << " ";

        const char* level_str = "";
        const char* color_code = "";
        const char* color_reset = "\033[0m";

        switch (level) {
            case LogLevel::Trace:
                level_str = "[TRACE]";
                color_code = "\033[90m"; // Gray
                break;
            case LogLevel::Debug:
                level_str = "[DEBUG]";
                color_code = "\033[36m"; // Cyan
                break;
            case LogLevel::Info:
                level_str = "[INFO ]";
                color_code = "\033[32m"; // Green
                break;
            case LogLevel::Warn:
                level_str = "[WARN ]";
                color_code = "\033[33m"; // Yellow
                break;
            case LogLevel::Error:
                level_str = "[ERROR]";
                color_code = "\033[31m"; // Red
                break;
            case LogLevel::Fatal:
                level_str = "[FATAL]";
                color_code = "\033[35m"; // Magenta
                break;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (colored_) {
            std::cout << color_code << ss.str() << level_str << " " << message << color_reset << std::endl;
        } else {
            std::cout << ss.str() << level_str << " " << message << std::endl;
        }
    }

private:
    Logger() : current_level_(LogLevel::Info), colored_(true) {}
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    LogLevel current_level_;
    bool colored_;
    std::mutex mutex_;
};

#define LOG_TRACE(msg) do { \
    std::ostringstream _oss; _oss << msg; \
    audiorouter::Logger::instance().log(audiorouter::LogLevel::Trace, _oss.str()); \
} while(0)

#define LOG_DEBUG(msg) do { \
    std::ostringstream _oss; _oss << msg; \
    audiorouter::Logger::instance().log(audiorouter::LogLevel::Debug, _oss.str()); \
} while(0)

#define LOG_INFO(msg) do { \
    std::ostringstream _oss; _oss << msg; \
    audiorouter::Logger::instance().log(audiorouter::LogLevel::Info, _oss.str()); \
} while(0)

#define LOG_WARN(msg) do { \
    std::ostringstream _oss; _oss << msg; \
    audiorouter::Logger::instance().log(audiorouter::LogLevel::Warn, _oss.str()); \
} while(0)

#define LOG_ERROR(msg) do { \
    std::ostringstream _oss; _oss << msg; \
    audiorouter::Logger::instance().log(audiorouter::LogLevel::Error, _oss.str()); \
} while(0)

#define LOG_FATAL(msg) do { \
    std::ostringstream _oss; _oss << msg; \
    audiorouter::Logger::instance().log(audiorouter::LogLevel::Fatal, _oss.str()); \
} while(0)

} // namespace audiorouter
