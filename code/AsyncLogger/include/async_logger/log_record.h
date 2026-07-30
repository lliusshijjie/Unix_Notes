#pragma once

#include "async_logger/log_level.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <thread>

template <std::size_t Capacity>
struct FixedText {
    static_assert(Capacity > 0, "FixedText capacity must be positive");

    std::array<char, Capacity> data{};
    std::uint16_t size{0};

    void assign(std::string_view value) noexcept {
        const std::size_t copied = std::min(value.size(), Capacity - 1);
        std::copy_n(value.data(), copied, data.data());
        data[copied] = '\0';
        size = static_cast<std::uint16_t>(copied);
    }

    std::string_view view() const noexcept {
        return {data.data(), size};
    }
};

inline constexpr std::size_t kMaxLogMessageSize = 512;
inline constexpr std::size_t kMaxLogFileNameSize = 192;
inline constexpr std::size_t kMaxLogFunctionNameSize = 128;
inline constexpr std::size_t kFormattedLogRecordSize = 1024;

struct LogRecord {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level{LogLevel::Info};
    std::uint64_t thread_id{0};
    FixedText<kMaxLogFileNameSize> file;
    int line{0};
    FixedText<kMaxLogFunctionNameSize> function;
    FixedText<kMaxLogMessageSize> message;
    std::uint64_t sequence{0};
};
