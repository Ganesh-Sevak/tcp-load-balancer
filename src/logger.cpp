#include "lb/logger.hpp"

#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace lb {
namespace {

std::atomic<int> min_level{static_cast<int>(LogLevel::Info)};
std::mutex log_mutex;

const char* label(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
    }
    return "UNKNOWN";
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

}  // namespace

void set_log_level(LogLevel level) {
    min_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

void log(LogLevel level, const std::string& message) {
    if (static_cast<int>(level) < min_level.load(std::memory_order_relaxed)) {
        return;
    }

    std::lock_guard<std::mutex> lock(log_mutex);
    std::cerr << "{\"ts\":\"" << timestamp() << "\",\"level\":\"" << label(level)
              << "\",\"message\":\"" << message << "\"}\n";
}

}  // namespace lb

