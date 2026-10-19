#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace apivpn {

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error,
    Off
};

class Logger {
public:
    using LogCallback = std::function<void(LogLevel, const std::string&)>;

    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void setTag(const std::string& tag) {
        std::lock_guard<std::mutex> lock(mutex_);
        tag_ = tag;
    }

    // Suppress messages below `level` (useful to keep test/automation output readable)
    void setMinLevel(LogLevel level) {
        std::lock_guard<std::mutex> lock(mutex_);
        min_level_ = level;
    }

    void setCallback(LogCallback cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(cb);
    }

    void log(LogLevel level, const std::string& msg) {
        std::string formatted;
        LogCallback cb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (level < min_level_) return;
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch()) % 1000;
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            std::tm tm_buf{};
#if defined(_WIN32)
            localtime_s(&tm_buf, &t);
#else
            localtime_r(&t, &tm_buf);
#endif
            std::ostringstream oss;
            oss << std::put_time(&tm_buf, "%H:%M:%S")
                << "." << std::setfill('0') << std::setw(3) << ms.count()
                << " [" << levelToString(level) << "]";
            if (!tag_.empty()) {
                oss << " [" << tag_ << "]";
            }
            oss << " " << msg;
            formatted = oss.str();
            cb = callback_;
            std::cout << formatted << std::endl;
        }
        if (cb) {
            cb(level, formatted);
        }
    }

private:
    Logger() = default;

    static const char* levelToString(LogLevel level) {
        switch (level) {
            case LogLevel::Debug: return "DBG";
            case LogLevel::Info:  return "INF";
            case LogLevel::Warn:  return "WRN";
            case LogLevel::Error: return "ERR";
            case LogLevel::Off:   return "OFF";
        }
        return "INF";
    }

    std::mutex mutex_;
    std::string tag_;
    LogLevel min_level_ = LogLevel::Info;
    LogCallback callback_;
};

inline void logInfo(const std::string& msg)  { Logger::instance().log(LogLevel::Info, msg); }
inline void logWarn(const std::string& msg)  { Logger::instance().log(LogLevel::Warn, msg); }
inline void logError(const std::string& msg) { Logger::instance().log(LogLevel::Error, msg); }
inline void logDebug(const std::string& msg) { Logger::instance().log(LogLevel::Debug, msg); }

} // namespace apivpn
