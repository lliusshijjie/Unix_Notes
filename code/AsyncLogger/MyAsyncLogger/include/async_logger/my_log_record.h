#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "async_logger/log_level.h"

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