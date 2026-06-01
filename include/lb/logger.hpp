#pragma once

#include <string>

namespace lb {

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};

void set_log_level(LogLevel level);
void log(LogLevel level, const std::string& message);

}  // namespace lb

