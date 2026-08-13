// 日志轮子（AsyncLogger）集成冒烟测试。
#include "base/Logger.h"

#include <cassert>
#include <chrono>
#include <filesystem>

int main() {
    using namespace std::chrono_literals;

    LoggerConfig config;
    config.file_path = "logs/logger_test.log";
    config.min_level = LogLevel::Trace;
    config.flush_interval = 50ms;
    minireactor::Logger::init(config);

    MR_LOG_TRACE("trace message");
    MR_LOG_DEBUG("debug message");
    MR_LOG_INFO("info message");
    MR_LOG_WARN("warn message");
    MR_LOG_ERROR("error message");
    MR_LOG_FATAL("fatal message");

    minireactor::Logger::instance().asyncLogger().flush();
    assert(minireactor::Logger::instance().asyncLogger().written_count() == 6);
    assert(std::filesystem::exists("logs/logger_test.log"));
    return 0;
}
