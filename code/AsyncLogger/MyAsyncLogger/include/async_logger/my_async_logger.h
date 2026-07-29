#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "async_logger/log_level.h"
#include "async_logger/log_record.h"

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
};

class AsyncLogger {
public:
    explicit AsyncLogger(LoggerConfig config);
    ~AsyncLogger() noexcept;

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;
    AsyncLogger(const AsyncLogger&&) = delete;
    AsyncLogger& operator=(const AsyncLogger&&) = delete;

    void start();
    
    bool log(LogLevel level, 
             std::string message,
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

    void work_entry() noexcept;
    void work_loop();
    void fail_work(const char* message) noexcept;
    void write_record(const LogRecord& record);
    void rotate_if_needed(std::size_t next_record_size);
    void flush_output();
    std::string format_record(const LogRecord& record) noexcept;
    void validate_config() const;

    LoggerConfig config_;

    mutable std::mutex mutex_;
    std::mutex lifecycle_mutex_;
    std::condition_variable data_cv_;
    std::condition_variable space_cv_;
    std::condition_variable flush_cv_;
    std::deque<LogRecord> queue_;
    std::thread worker_;
    State state_{State::Created};

    std::ofstream output_;
    std::uintmax_t current_file_size_{0};
    std::uint64_t next_sequence_{0};
    std::uint64_t last_written_sequence_{0};
    std::uint64_t last_flushed_sequence_{0};
    std::uint64_t flush_target_sequence_{0};
    bool flush_requested_{false};

    std::atomic<std::uint64_t> accepted_count_{0};
    std::atomic<std::uint64_t> written_count_{0};
    std::atomic<std::uint64_t> dropped_count_{0};
    std::atomic<bool> has_error_{false};
};

#define ASYNC_LOG(logger, level, message) \
    ((logger).log((level), (message), __FILE__, __LINE__, __func__))

#define LOG_TRACE(logger, message) ASYNC_LOG((logger), LogLevel::Trace, (message))
#define LOG_DEBUG(logger, message) ASYNC_LOG((logger), LogLevel::Debug, (message))
#define LOG_INFO(logger, message) ASYNC_LOG((logger), LogLevel::Info, (message))
#define LOG_WARN(logger, message) ASYNC_LOG((logger), LogLevel::Warn, (message))
#define LOG_ERROR(logger, message) ASYNC_LOG((logger), LogLevel::Error, (message))
#define LOG_FATAL(logger, message) ASYNC_LOG((logger), LogLevel::Fatal, (message))





