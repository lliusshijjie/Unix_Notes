#pragma once

#include "async_logger/log_level.h"
#include "async_logger/log_record.h"
#include "async_logger/mpsc_ring_buffer.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

enum class OverflowPolicy {
    Block,
    DropNewest
};

struct LoggerConfig {
    std::filesystem::path file_path;
    LogLevel min_level = LogLevel::Info;
    std::size_t max_queue_size = 8192;
    OverflowPolicy overflow_policy = OverflowPolicy::Block;
    std::chrono::milliseconds flush_interval{1000};
    std::uintmax_t max_file_size = 100 * 1024 * 1024;
    std::size_t max_backup_files = 5;
    bool flush_on_error = false;
    std::size_t batch_buffer_size = 64 * 1024;
};

class AsyncLogger {
public:
    explicit AsyncLogger(LoggerConfig config);
    ~AsyncLogger() noexcept;

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;
    AsyncLogger(AsyncLogger&&) = delete;
    AsyncLogger& operator=(AsyncLogger&&) = delete;

    void start();

    bool log(LogLevel level,
             std::string_view message,
             const char* file,
             int line,
             const char* function);

    void flush();
    void stop();

    bool is_running() const noexcept;
    bool has_error() const noexcept;

    std::uint64_t accepted_count() const noexcept;
    std::uint64_t written_count() const noexcept;
    std::uint64_t dropped_count() const noexcept;

private:
    enum class State {
        Created,
        Running,
        Stopping,
        Stopped,
        Failed
    };

    void worker_entry() noexcept;
    void worker_loop();
    void fail_worker(const char* message) noexcept;
    void append_record(const LogRecord& record);
    void write_active_buffer();
    void flush_output();
    void rotate_if_needed(std::size_t next_record_size);
    std::size_t format_record(const LogRecord& record, char* destination,
                              std::size_t capacity);
    void validate_config() const;
    void notify_worker() noexcept;
    void finish_producer() noexcept;

    LoggerConfig config_;
    MpscRingBuffer<LogRecord> queue_;

    std::mutex lifecycle_mutex_;
    std::mutex wake_mutex_;
    std::mutex space_mutex_;
    std::mutex flush_mutex_;
    std::condition_variable data_cv_;
    std::condition_variable space_cv_;
    std::condition_variable flush_cv_;
    std::thread worker_;
    std::atomic<State> state_{State::Created};
    std::atomic<std::size_t> pending_count_{0};
    std::atomic<std::size_t> active_producers_{0};

    std::ofstream output_;
    std::uintmax_t current_file_size_{0};
    std::vector<char> active_buffer_;
    std::vector<char> write_buffer_;
    std::size_t active_buffer_size_{0};
    std::size_t active_record_count_{0};
    std::time_t cached_second_{0};
    std::array<char, 20> cached_time_text_{};
    bool has_cached_time_{false};

    std::atomic<std::uint64_t> next_sequence_{0};
    std::atomic<std::uint64_t> accepted_count_{0};
    std::atomic<std::uint64_t> written_count_{0};
    std::atomic<std::uint64_t> last_flushed_count_{0};
    std::atomic<std::uint64_t> flush_target_count_{0};
    std::atomic<bool> flush_requested_{false};
    std::atomic<std::uint64_t> dropped_count_{0};
    std::atomic<bool> has_error_{false};
};

#ifndef ASYNC_LOG_COMPILED_MIN_LEVEL
#define ASYNC_LOG_COMPILED_MIN_LEVEL 0
#endif

#define ASYNC_LOG(logger, level, message) \
    ((logger).log((level), (message), __FILE__, __LINE__, __func__))

#if ASYNC_LOG_COMPILED_MIN_LEVEL <= 0
#define LOG_TRACE(logger, message) ASYNC_LOG((logger), LogLevel::Trace, (message))
#else
#define LOG_TRACE(logger, message) ((void)0)
#endif

#if ASYNC_LOG_COMPILED_MIN_LEVEL <= 1
#define LOG_DEBUG(logger, message) ASYNC_LOG((logger), LogLevel::Debug, (message))
#else
#define LOG_DEBUG(logger, message) ((void)0)
#endif

#if ASYNC_LOG_COMPILED_MIN_LEVEL <= 2
#define LOG_INFO(logger, message) ASYNC_LOG((logger), LogLevel::Info, (message))
#else
#define LOG_INFO(logger, message) ((void)0)
#endif

#if ASYNC_LOG_COMPILED_MIN_LEVEL <= 3
#define LOG_WARN(logger, message) ASYNC_LOG((logger), LogLevel::Warn, (message))
#else
#define LOG_WARN(logger, message) ((void)0)
#endif

#if ASYNC_LOG_COMPILED_MIN_LEVEL <= 4
#define LOG_ERROR(logger, message) ASYNC_LOG((logger), LogLevel::Error, (message))
#else
#define LOG_ERROR(logger, message) ((void)0)
#endif

#if ASYNC_LOG_COMPILED_MIN_LEVEL <= 5
#define LOG_FATAL(logger, message) ASYNC_LOG((logger), LogLevel::Fatal, (message))
#else
#define LOG_FATAL(logger, message) ((void)0)
#endif
