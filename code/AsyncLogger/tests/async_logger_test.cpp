#include "async_logger/async_logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* name) {
    if (condition) {
        std::cout << "[PASS] " << name << '\n';
    } else {
        std::cout << "[FAIL] " << name << '\n';
        ++failures;
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::size_t line_count(const std::string& content) {
    return static_cast<std::size_t>(std::count(content.begin(), content.end(), '\n'));
}

void test_filter_flush_and_stop(const std::filesystem::path& path) {
    LoggerConfig config;
    config.file_path = path;
    config.min_level = LogLevel::Warn;
    config.flush_interval = std::chrono::milliseconds(10);

    AsyncLogger logger(config);
    logger.start();
    expect(LOG_INFO(logger, "filtered") , "Filtered log returns success");
    expect(LOG_WARN(logger, "warning") , "Warn log accepted");
    expect(LOG_ERROR(logger, "failure") , "Error log accepted");
    logger.flush();
    logger.stop();
    logger.stop();

    const std::string content = read_file(path);
    expect(content.find("filtered") == std::string::npos, "Filtered log not written");
    expect(content.find("[WARN]") != std::string::npos, "Warn level written");
    expect(content.find("[ERROR]") != std::string::npos, "Error level written");
    expect(logger.accepted_count() == 2 && logger.written_count() == 2,
           "Accepted records are written before stop");
}

void test_concurrent_logging(const std::filesystem::path& path) {
    LoggerConfig config;
    config.file_path = path;
    config.min_level = LogLevel::Trace;
    config.max_queue_size = 32;
    config.overflow_policy = OverflowPolicy::Block;
    config.flush_interval = std::chrono::milliseconds(10);

    AsyncLogger logger(config);
    logger.start();

    constexpr int worker_count = 8;
    constexpr int records_per_worker = 1000;
    std::atomic<int> accepted{0};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&logger, &accepted, worker] {
            for (int index = 0; index < records_per_worker; ++index) {
                if (LOG_INFO(logger, "worker=" + std::to_string(worker) +
                                         ", record=" + std::to_string(index))) {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    logger.stop();
    expect(accepted.load() == worker_count * records_per_worker, "Concurrent logs accepted");
    expect(logger.accepted_count() == logger.written_count(), "Concurrent logs drained on stop");
    expect(line_count(read_file(path)) == logger.written_count(),
           "MPSC stress test writes every accepted record once");
}

void test_fixed_record_and_timestamp_format(const std::filesystem::path& path) {
    LoggerConfig config;
    config.file_path = path;
    config.min_level = LogLevel::Trace;
    config.flush_interval = std::chrono::milliseconds(10);

    AsyncLogger logger(config);
    logger.start();
    const std::string oversized_message(1024, '~');
    expect(LOG_INFO(logger, oversized_message), "Fixed-buffer record accepted");
    logger.flush();
    logger.stop();

    const std::string content = read_file(path);
    const std::size_t marker_count =
        static_cast<std::size_t>(std::count(content.begin(), content.end(), '~'));
    expect(marker_count == kMaxLogMessageSize - 1,
           "Oversized message is safely truncated to fixed buffer capacity");
    expect(content.size() >= 26 && content[4] == '-' && content[7] == '-' &&
               content[10] == ' ' && content[19] == '.',
           "Log timestamp has cached date prefix and microsecond format");
}

void test_drop_newest_with_mpsc_contention(const std::filesystem::path& path) {
    LoggerConfig config;
    config.file_path = path;
    config.min_level = LogLevel::Trace;
    config.max_queue_size = 2;
    config.overflow_policy = OverflowPolicy::DropNewest;
    config.flush_interval = std::chrono::milliseconds(10);

    AsyncLogger logger(config);
    logger.start();
    constexpr int worker_count = 12;
    constexpr int records_per_worker = 1000;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&logger] {
            for (int index = 0; index < records_per_worker; ++index) {
                (void)LOG_DEBUG(logger, "drop-contention");
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    logger.stop();

    const std::uint64_t total = static_cast<std::uint64_t>(worker_count) * records_per_worker;
    expect(logger.accepted_count() + logger.dropped_count() == total,
           "DropNewest accounts for every submitted record");
    expect(logger.dropped_count() > 0, "DropNewest drops records when MPSC ring is saturated");
}

void test_rotation(const std::filesystem::path& path) {
    LoggerConfig config;
    config.file_path = path;
    config.min_level = LogLevel::Trace;
    config.max_file_size = 140;
    config.max_backup_files = 2;
    config.flush_interval = std::chrono::milliseconds(10);

    AsyncLogger logger(config);
    logger.start();
    for (int index = 0; index < 8; ++index) {
        LOG_INFO(logger, "roll-record-" + std::to_string(index));
    }
    logger.stop();

    expect(std::filesystem::exists(path), "Current rotated log exists");
    expect(std::filesystem::exists(std::filesystem::path(path.string() + ".1")),
           "First log backup exists");
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("my_async_logger_test_" + std::to_string(std::rand()));
    std::filesystem::create_directories(root);

    test_filter_flush_and_stop(root / "filter.log");
    test_concurrent_logging(root / "concurrent.log");
    test_fixed_record_and_timestamp_format(root / "fixed.log");
    test_drop_newest_with_mpsc_contention(root / "drop.log");
    test_rotation(root / "rotate.log");

    std::error_code error;
    std::filesystem::remove_all(root, error);

    if (failures == 0) {
        std::cout << "ALL PASSED\n";
        return 0;
    }
    std::cout << failures << " TEST(S) FAILED\n";
    return 1;
}
