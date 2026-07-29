#pragma once

#include <string_view>

enum class LogLevel {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

std::string_view to_string(LogLevel level) noexcept;
