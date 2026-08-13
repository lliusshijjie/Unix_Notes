#pragma once

#include "async_logger/async_logger.h"

#include <memory>
#include <string_view>

namespace minireactor {

// 异步日志门面：封装上一级目录的 AsyncLogger 轮子。
//
// 用法：
//   minireactor::Logger::init(config);   // 可选；必须在第一次日志调用之前执行
//   MR_LOG_INFO("server listening " + std::to_string(port));
//
// 未调用 init() 时使用默认配置（logs/minireactor.log，Debug 级别）。
class Logger {
public:
    static void init(LoggerConfig config);
    static Logger& instance();

    void log(LogLevel level, std::string_view message, const char* file, int line,
             const char* function);
    AsyncLogger& asyncLogger() noexcept;

    static LoggerConfig defaultConfig();

private:
    Logger() = default;

    std::unique_ptr<AsyncLogger> async_;
};

}  // namespace minireactor

#define MR_LOG_TRACE(message)                                                                     \
    ::minireactor::Logger::instance().log(LogLevel::Trace, (message), __FILE__, __LINE__,         \
                                          __func__)
#define MR_LOG_DEBUG(message)                                                                     \
    ::minireactor::Logger::instance().log(LogLevel::Debug, (message), __FILE__, __LINE__,         \
                                          __func__)
#define MR_LOG_INFO(message)                                                                      \
    ::minireactor::Logger::instance().log(LogLevel::Info, (message), __FILE__, __LINE__,          \
                                          __func__)
#define MR_LOG_WARN(message)                                                                      \
    ::minireactor::Logger::instance().log(LogLevel::Warn, (message), __FILE__, __LINE__,          \
                                          __func__)
#define MR_LOG_ERROR(message)                                                                     \
    ::minireactor::Logger::instance().log(LogLevel::Error, (message), __FILE__, __LINE__,         \
                                          __func__)
#define MR_LOG_FATAL(message)                                                                     \
    ::minireactor::Logger::instance().log(LogLevel::Fatal, (message), __FILE__, __LINE__,         \
                                          __func__)
