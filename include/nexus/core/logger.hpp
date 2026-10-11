#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>

namespace nexus::core {

enum class LogLevel {
    kDebug = 0,
    kInfo = 1,
    kWarn = 2,
    kError = 3,
    kSilent = 4
};

class Logger {
public:
    static Logger& Instance() {
        static Logger instance;
        return instance;
    }

    void SetLevel(LogLevel level) noexcept { level_ = level; }
    LogLevel GetLevel() const noexcept { return level_; }

    template <typename... Args>
    void Log(LogLevel level, std::string_view file, int line, Args&&... args) {
        if (level < level_) {
            return;
        }

        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) %
                  1000;
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
        localtime_r(&now_c, &tm_buf);

        // Extract basename
        auto pos = file.find_last_of("/\\");
        std::string_view short_file = (pos == std::string_view::npos) ? file : file.substr(pos + 1);

        std::ostringstream oss;
        oss << "[" << std::put_time(&tm_buf, "%H:%M:%S") << "."
            << std::setfill('0') << std::setw(3) << ms.count() << "] "
            << "[" << LevelToString(level) << "] "
            << "[" << short_file << ":" << line << "] ";
        (oss << ... << std::forward<Args>(args));
        oss << "\n";

        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << oss.str() << std::flush;
    }

private:
    Logger() = default;

    static constexpr std::string_view LevelToString(LogLevel level) noexcept {
        switch (level) {
            case LogLevel::kDebug: return "DEBUG";
            case LogLevel::kInfo:  return "INFO ";
            case LogLevel::kWarn:  return "WARN ";
            case LogLevel::kError: return "ERROR";
            default:               return "UNK  ";
        }
    }

    LogLevel level_ = LogLevel::kInfo;
    std::mutex mutex_;
};

}  // namespace nexus::core

#define LOG_DEBUG(...) ::nexus::core::Logger::Instance().Log(::nexus::core::LogLevel::kDebug, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  ::nexus::core::Logger::Instance().Log(::nexus::core::LogLevel::kInfo,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  ::nexus::core::Logger::Instance().Log(::nexus::core::LogLevel::kWarn,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) ::nexus::core::Logger::Instance().Log(::nexus::core::LogLevel::kError, __FILE__, __LINE__, __VA_ARGS__)
