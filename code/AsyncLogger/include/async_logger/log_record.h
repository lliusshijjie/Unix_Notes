#pragma once

#include "async_logger/log_level.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

struct LogRecord {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    std::thread::id thread_id;
    std::string file;
    int line;
    std::string function;
    std::string message;
    std::uint64_t sequence;
};
