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
        worker_ = std::thread(&AsyncLogger::worker_entry, this);
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
    if (level < config_.min_level) {
        return true;
    }

    LogRecord record {
        std::chrono::system_clock::now(),
        level,
        std::this_thread::get_id(),
        file == nullptr ? "" : file,
        line,
        function == nullptr ? "" : function,
        std::move(message),
        0,
    };

    bool should_flush = false;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (state_ != State::Running) {
            return false;
        }

        if (config_.overflow_policy == OverflowPolicy::Block) {
            space_cv_.wait(lock, [this] {
                return queue_.size() < config_.max_queue_size || state_ != State::Running;
            });
            if (state_ != State::Running) {
                return false;
            }
        } else if (queue_.size() >= config_.max_queue_size) {
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        record.sequence = next_sequence_++;
        queue_.push_back(std::move(record));
        accepted_count_.fetch_add(1, std::memory_order_relaxed);
        should_flush = config_.flush_on_error && level >= LogLevel::Error;
    }

    data_cv_.notify_one();
    if (should_flush) {
        flush();
    }
    return true;
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

void AsyncLogger::worker_entry() noexcept {
    try {
        worker_loop();
    } catch(const std::exception& error) {
        fail_worker(error.what());
    } catch(...) {
        fail_worker("unknown asynchronous logger error");
    }
} 

void AsyncLogger::fail_worker(const char* message) {
    std::cerr << "AsyncLogger worker error:" << message << "\n";
    has_error_.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = State::Failed;
    }
    data_cv_.notify_all();
    space_cv_.notify_all();
    flush_cv_.notify_all();
}

void AsyncLogger::worker_loop() {
    auto next_periodic_flush = std::chrono::steady_clock::now() + config_.flush_interval;
    
    for (;;) {
        std::optional<LogRecord> record;
        bool should_flush = false;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            data_cv_.wait_until(lock, next_periodic_flush, [this] {
                return !queue_.empty() || state_ != State::Running || flush_requested_;
            });

            if (!queue_.empty()) {
                record.emplace(std::move(queue_.front()));
                queue_.pop_front();
                space_cv_.notify_one();
            } else if (state_ == State::Stopping) {
                lock.unlock();
                flush_output();
                lock.lock();
                last_flushed_sequence_ = std::max(last_flushed_sequence_, last_written_sequence_);
                flush_requested_ = false;
                flush_cv_.notify_all();
                return;
            } else {
                should_flush = flush_requested_ || std::chrono::steady_clock::now() >= next_periodic_flush;
            }
        }

        if (record.has_value()) {
            write_record(*record);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                last_written_sequence_ = std::max(last_written_sequence_, record->sequence);
                written_count_.fetch_add(1, std::memory_order_relaxed);
                should_flush = flush_requested_ || last_written_sequence_ >= flush_target_sequence_;
            }
        }

        if (should_flush) {
            flush_output();
            std::lock_guard<std::mutex> lock(mutex_);
            last_flushed_sequence_ = std::max(last_flushed_sequence_, last_written_sequence_);
            if (flush_requested_ && last_written_sequence_ >= flush_target_sequence_) {
                flush_requested_ = false;
            }
            flush_cv_.notify_all();
        }

        if (std::chrono::steady_clock::now() >= next_periodic_flush) {
            if (!should_flush) {
                flush_output();
                std::lock_guard<std::mutex> lock(mutex_);
                last_flushed_sequence_ = std::max(last_flushed_sequence_, last_written_sequence_);
                flush_cv_.notify_all();
            }
            next_periodic_flush = std::chrono::steady_clock::now() + config_.flush_interval;
        }
    }
}

std::string AsyncLogger::format_record(const LogRecord& record) const {
    const auto seconds = std::chrono::system_clock::to_time_t(record.timestamp);
    const auto mircos = std::chrono::duration_cast<std::chrono::milliseconds>(
                            record.timestamp.time_since_epoch()) % 
                        std::chrono::seconds(1);

    const std::tm calendar_time = local_time(seconds);
    std::ostringstream stream;
    stream << std::put_time(&calendar_time, "%Y-%m-%d %H:%M:%S") << "."
           << std::setw(6) << std::setfill('0') << mircos.count() << std::setfill(' ')
           << " [" << to_string(record.level) << "] [tid =" << record.thread_id << "] ["
           << record.file << ':' << record.line << ' ' << record.function << "] "
           << record.message << '\n';
    return stream.str();
}

void AsyncLogger::write_record(const LogRecord& record) {
    const std::string formatted = format_record(record);
    rotate_if_needed(formatted.size());
    output_ << formatted;
    if (!output_) {
        throw std::runtime_error("failed to write log record");
    }
    current_file_size_ += formatted.size();
}

void AsyncLogger::flush_output() {
    output_.flush();
    if (!output_) {
        throw std::runtime_error("failed to fluh log file");
    }
}

void AsyncLogger::rotate_if_needed(std::size_t next_record_size) {
    if (config_.max_file_size == 0 || current_file_size_ == 0 ||
        next_record_size <= config_.max_file_size - std::min(current_file_size_, config_.max_file_size)) {
        return;
    }    

    flush_output();
    output_.close();
    if (config_.max_backup_files == 0) {
        output_.open(config_.file_path, std::ios::out | std::ios::trunc);
        if (!output_.is_open()) {
            throw std::runtime_error("failed to recreate log file after rotation");
        }
        current_file_size_ = 0;
        return;
    }

    std::error_code error;
    std::filesystem::remove(backup_path(config_.file_path, config_.max_backup_files), error);
    if (error) {
        throw std::runtime_error("failed to remove oldest log backup: " + error.message());
    }
    for (std::size_t index = config_.max_backup_files; index > 1; --index) {
        const auto source = backup_path(config_.file_path, index - 1);
        const auto destination = backup_path(config_.file_path, index);
        if (std::filesystem::exists(source, error)) {
            if (error) {
                throw std::runtime_error("failed to inspect log backup: " + error.message());
            }
            std::filesystem::rename(source, destination, error);
            if (error) {
                throw std::runtime_error("failed to rotate log backup: " + error.message());
            }
        } else if (error) {
            throw std::runtime_error("failed to inspect log backup: " + error.message());
        }
    }
    std::filesystem::rename(config_.file_path, backup_path(config_.file_path, 1), error);
    if (error) {
        throw std::runtime_error("failed to rotate current log file: " + error.message());
    }

    output_.open(config_.file_path, std::ios::out | std::ios::trunc);
    if (!output_.is_open()) {
        throw std::runtime_error("failed to create rotated log file");
    }
    current_file_size_ = 0;
}


