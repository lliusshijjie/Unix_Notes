#include "async_logger/async_logger.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

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

void update_max(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept {
    std::uint64_t current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value, std::memory_order_release,
                                         std::memory_order_relaxed)) {
    }
}

} // namespace

AsyncLogger::AsyncLogger(LoggerConfig config)
    : config_(std::move(config))
    , queue_(config_.max_queue_size) {}

AsyncLogger::~AsyncLogger() noexcept {
    try {
        stop();
    } catch (...) {
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
    if (config_.batch_buffer_size < kFormattedLogRecordSize) {
        throw std::invalid_argument("logger batch_buffer_size is too small");
    }
}

void AsyncLogger::start() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (state_.load(std::memory_order_acquire) != State::Created) {
        throw std::logic_error("AsyncLogger can only be started once");
    }

    validate_config();
    const std::filesystem::path parent = config_.file_path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) {
            throw std::runtime_error("failed to create log directory: " + error.message());
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
        active_buffer_.resize(config_.batch_buffer_size);
        write_buffer_.resize(config_.batch_buffer_size);
        state_.store(State::Running, std::memory_order_release);
        worker_ = std::thread(&AsyncLogger::worker_entry, this);
    } catch (...) {
        state_.store(State::Created, std::memory_order_release);
        output_.close();
        active_buffer_.clear();
        write_buffer_.clear();
        throw;
    }
}

bool AsyncLogger::log(LogLevel level,
                      std::string_view message,
                      const char* file,
                      int line,
                      const char* function) {
    if (level < config_.min_level) {
        return true;
    }
    if (state_.load(std::memory_order_acquire) != State::Running) {
        return false;
    }

    active_producers_.fetch_add(1, std::memory_order_acq_rel);
    const auto producer_done = [this] { finish_producer(); };
    if (state_.load(std::memory_order_acquire) != State::Running) {
        producer_done();
        return false;
    }

    LogRecord record;
    record.timestamp = std::chrono::system_clock::now();
    record.level = level;
    record.thread_id = static_cast<std::uint64_t>(std::hash<std::thread::id>{}(
        std::this_thread::get_id()));
    record.file.assign(file == nullptr ? std::string_view{} : std::string_view(file));
    record.line = line;
    record.function.assign(function == nullptr ? std::string_view{} : std::string_view(function));
    record.message.assign(message);
    record.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;

    std::size_t previous_pending = 0;
    for (;;) {
        previous_pending = pending_count_.fetch_add(1, std::memory_order_acq_rel);
        if (queue_.try_enqueue(std::move(record))) {
            break;
        }
        pending_count_.fetch_sub(1, std::memory_order_release);
        if (config_.overflow_policy == OverflowPolicy::DropNewest) {
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
            producer_done();
            return false;
        }

        std::unique_lock<std::mutex> lock(space_mutex_);
        space_cv_.wait(lock, [this] {
            return state_.load(std::memory_order_acquire) != State::Running || !queue_.full();
        });
        if (state_.load(std::memory_order_acquire) != State::Running) {
            producer_done();
            return false;
        }
    }

    accepted_count_.fetch_add(1, std::memory_order_release);
    producer_done();
    if (previous_pending == 0) {
        notify_worker();
    }

    if (config_.flush_on_error && level >= LogLevel::Error) {
        flush();
    }
    return true;
}

void AsyncLogger::flush() {
    if (state_.load(std::memory_order_acquire) == State::Created ||
        state_.load(std::memory_order_acquire) == State::Stopped ||
        state_.load(std::memory_order_acquire) == State::Failed) {
        return;
    }

    const std::uint64_t target = accepted_count_.load(std::memory_order_acquire);
    update_max(flush_target_count_, target);
    flush_requested_.store(true, std::memory_order_release);
    notify_worker();

    std::unique_lock<std::mutex> lock(flush_mutex_);
    flush_cv_.wait(lock, [this, target] {
        const State state = state_.load(std::memory_order_acquire);
        return last_flushed_count_.load(std::memory_order_acquire) >= target ||
               state == State::Failed || state == State::Stopped;
    });
}

void AsyncLogger::stop() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    State current = state_.load(std::memory_order_acquire);
    if (current == State::Created) {
        state_.store(State::Stopped, std::memory_order_release);
        return;
    }
    if (current == State::Stopped) {
        return;
    }
    if (current == State::Running) {
        state_.store(State::Stopping, std::memory_order_release);
    }

    notify_worker();
    space_cv_.notify_all();
    flush_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (output_.is_open()) {
        output_.close();
    }

    if (state_.load(std::memory_order_acquire) != State::Failed) {
        state_.store(State::Stopped, std::memory_order_release);
    }
    flush_cv_.notify_all();
}

bool AsyncLogger::is_running() const noexcept {
    return state_.load(std::memory_order_acquire) == State::Running;
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

void AsyncLogger::notify_worker() noexcept {
    std::lock_guard<std::mutex> lock(wake_mutex_);
    data_cv_.notify_one();
}

void AsyncLogger::finish_producer() noexcept {
    active_producers_.fetch_sub(1, std::memory_order_release);
    if (state_.load(std::memory_order_acquire) != State::Running) {
        notify_worker();
    }
}

void AsyncLogger::worker_entry() noexcept {
    try {
        worker_loop();
    } catch (const std::exception& error) {
        fail_worker(error.what());
    } catch (...) {
        fail_worker("unknown asynchronous logger error");
    }
}

void AsyncLogger::fail_worker(const char* message) noexcept {
    std::cerr << "AsyncLogger worker error: " << message << '\n';
    has_error_.store(true, std::memory_order_relaxed);
    state_.store(State::Failed, std::memory_order_release);
    notify_worker();
    space_cv_.notify_all();
    flush_cv_.notify_all();
}

void AsyncLogger::worker_loop() {
    auto next_periodic_flush = std::chrono::steady_clock::now() + config_.flush_interval;

    for (;;) {
        LogRecord record;
        bool consumed = false;
        while (queue_.try_dequeue(record)) {
            pending_count_.fetch_sub(1, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(space_mutex_);
                space_cv_.notify_one();
            }
            append_record(record);
            consumed = true;
        }

        const State current = state_.load(std::memory_order_acquire);
        const bool stopping_and_drained =
            current == State::Stopping && pending_count_.load(std::memory_order_acquire) == 0 &&
            active_producers_.load(std::memory_order_acquire) == 0;
        const bool flush_due = flush_requested_.load(std::memory_order_acquire) ||
                               std::chrono::steady_clock::now() >= next_periodic_flush ||
                               stopping_and_drained;

        if (flush_due) {
            write_active_buffer();
            flush_output();
            const std::uint64_t written = written_count_.load(std::memory_order_acquire);
            last_flushed_count_.store(written, std::memory_order_release);
            if (written >= flush_target_count_.load(std::memory_order_acquire)) {
                flush_requested_.store(false, std::memory_order_release);
            }
            flush_cv_.notify_all();
            next_periodic_flush = std::chrono::steady_clock::now() + config_.flush_interval;
        }

        if (stopping_and_drained) {
            return;
        }
        if (current == State::Failed) {
            return;
        }
        if (consumed || pending_count_.load(std::memory_order_acquire) != 0) {
            continue;
        }

        std::unique_lock<std::mutex> lock(wake_mutex_);
        data_cv_.wait_until(lock, next_periodic_flush, [this] {
            return pending_count_.load(std::memory_order_acquire) != 0 ||
                   flush_requested_.load(std::memory_order_acquire) ||
                   state_.load(std::memory_order_acquire) != State::Running;
        });
    }
}

std::size_t AsyncLogger::format_record(const LogRecord& record, char* destination,
                                       std::size_t capacity) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(record.timestamp);
    if (!has_cached_time_ || seconds != cached_second_) {
        const std::tm calendar_time = local_time(seconds);
        if (std::strftime(cached_time_text_.data(), cached_time_text_.size(), "%Y-%m-%d %H:%M:%S",
                          &calendar_time) == 0) {
            throw std::runtime_error("failed to format log timestamp");
        }
        cached_second_ = seconds;
        has_cached_time_ = true;
    }

    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            record.timestamp.time_since_epoch()) %
                        std::chrono::seconds(1);
    const std::string_view level_text = to_string(record.level);
    const int result = std::snprintf(
        destination, capacity, "%s.%06lld [%.*s] [tid=%llu] [%.*s:%d %.*s] %.*s\n",
        cached_time_text_.data(), static_cast<long long>(micros.count()),
        static_cast<int>(level_text.size()), level_text.data(),
        static_cast<unsigned long long>(record.thread_id),
        static_cast<int>(record.file.view().size()), record.file.view().data(), record.line,
        static_cast<int>(record.function.view().size()), record.function.view().data(),
        static_cast<int>(record.message.view().size()), record.message.view().data());
    if (result < 0) {
        throw std::runtime_error("failed to format log record");
    }
    if (static_cast<std::size_t>(result) >= capacity) {
        destination[capacity - 2] = '\n';
        destination[capacity - 1] = '\0';
        return capacity - 1;
    }
    return static_cast<std::size_t>(result);
}

void AsyncLogger::append_record(const LogRecord& record) {
    std::array<char, kFormattedLogRecordSize> formatted{};
    const std::size_t formatted_size = format_record(record, formatted.data(), formatted.size());
    rotate_if_needed(formatted_size);
    if (active_buffer_size_ + formatted_size > active_buffer_.size()) {
        write_active_buffer();
    }
    std::memcpy(active_buffer_.data() + active_buffer_size_, formatted.data(), formatted_size);
    active_buffer_size_ += formatted_size;
    ++active_record_count_;
}

void AsyncLogger::write_active_buffer() {
    if (active_buffer_size_ == 0) {
        return;
    }
    std::swap(active_buffer_, write_buffer_);
    const std::size_t write_size = active_buffer_size_;
    const std::size_t write_records = active_record_count_;
    active_buffer_size_ = 0;
    active_record_count_ = 0;

    output_.write(write_buffer_.data(), static_cast<std::streamsize>(write_size));
    if (!output_) {
        throw std::runtime_error("failed to write log batch");
    }
    current_file_size_ += write_size;
    written_count_.fetch_add(write_records, std::memory_order_release);
}

void AsyncLogger::flush_output() {
    output_.flush();
    if (!output_) {
        throw std::runtime_error("failed to flush log file");
    }
}

void AsyncLogger::rotate_if_needed(std::size_t next_record_size) {
    if (config_.max_file_size == 0 ||
        (current_file_size_ == 0 && active_buffer_size_ == 0) ||
        (current_file_size_ <= config_.max_file_size &&
         active_buffer_size_ <= config_.max_file_size - current_file_size_ &&
         next_record_size <= config_.max_file_size - current_file_size_ - active_buffer_size_)) {
        return;
    }

    write_active_buffer();
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
