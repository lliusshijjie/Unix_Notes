#include <algorithm>
#include <ctime>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "async_logger/my_async_logger.h"

namespace {

std::tm local_time(std::time_t value) {
    std::tm result{};
#ifdef _WIN32
    if (localtime_s(&result, &value) != 0) {
        throw std::runtime_error("failed to convert log timestamp to local time");
    }
#else
    if (localtime_r(&value, &result) == nullptr) {
        throw std::runtime_error("failed to convert log timestamp to local time");
    }
#endif
    return result;
}

std::filesystem::path backup_path(const std::filesystem::path& path, std::size_t index) {
    return std::filesystem::path(path.string() + "." + std::to_string(index));
}

} // namespace

AsyncLogger::AsyncLogger(LoggerConfig config) 
    : config_(std::move(config)) {}

AsyncLogger::~AsyncLogger() noexcept {
    try {
        stop();
    } catch(...) { 
    }
}

void AsyncLogger::validate_config() const {
    if (config_.file_path.empty()) {
        throw std::invalid_argument("logger file_path must not be empty");
    }
    if (config_.max_queue_size == 0) {
        throw std::invalid_argument("logger max_queue_size must be greater than 0");
    }
    if (config_.flush_interval <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("logger flush_interval must be positive");
    }
}

void AsyncLogger::start() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::Created) {
            throw std::logic_error("AsyncLogger can only be started once");
        }
    }

    validate_config();

    const std::filesystem::path parent = config_.file_path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) {
            throw std::runtime_error("failed to create log diretory: " + error.message());
        }
    }

    std::error_code size_error;
    const bool exists = std::filesystem::exists(config_.file_path, size_error);
    if (size_error) {
        throw std::runtime_error("failed to inspect log file: " + size_error.message());
    }
    current_file_size_ = exists ? std::filesystem::file_size(config_.file_path, size_error) : 0;
    if (size_error) {
        throw std::runtime_error("failed to read log file size: " + size_error.message());
    }

    output_.open(config_.file_path, std::ios::out | std::ios::app);
    if (!output_.is_open()) {
        throw std::runtime_error("failed to open log file: " + config_.file_path.string());
    }

    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = State::Running;
        }
        worker_ = std::thread(&AsyncLogger::work_entry, this);
    } catch(...) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = State::Created;
        }
        output_.close();
        throw;
    }
}

bool AsyncLogger::log(LogLevel level,
                      std::string message,
                      const char* file,
                      int line,
                      const char* function) {

}

void AsyncLogger::flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ == State::Created || state_ == State::Stopped) {
        return;
    }

    const std::uint64_t target = next_sequence_;
    if (state_ == State::Failed) {
        return;
    }

    flush_target_sequence_ = std::max(flush_target_sequence_, target);
    flush_requested_ = true;
    lock.unlock();
    data_cv_.notify_one();
    lock.lock();

    flush_cv_.wait(lock, [this, target] {
        return last_flushed_sequence_ >= target || state_ == State::Failed || 
               state_ == State::Stopped;
    });
}

void AsyncLogger::stop() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

    bool join_worker = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::Created) {
            state_ = State::Stopped;
            return;
        }
        if (state_ == State::Stopped) {
            return;
        }
        if (state_ == State::Running) {
            state_ = State::Stopping;
        }
        join_worker = worker_.joinable();
    }

    data_cv_.notify_all();
    space_cv_.notify_all();
    flush_cv_.notify_all();

    if (join_worker) {
        worker_.join();
    }

    if (output_.is_open()) {
        output_.close();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Failed) {
        state_ = State::Stopped;
    }
    flush_cv_.notify_all();
}

bool AsyncLogger::is_running() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::Running;
}

bool AsyncLogger::has_error() const noexcept {
    return has_error_.load(std::memory_order_relaxed);
}

std::uint64_t AsyncLogger::accepted_count() const noexcept {
    return accepted_count_.load(std::memory_order_relaxed);
}

std::uint64_t AsyncLogger::written_count() const noexcept {
    return written_count_.load(std::memory_order_relaxed);
}

std::uint64_t AsyncLogger::dropped_count() const noexcept {
    return dropped_count_.load(std::memory_order_relaxed);
}

