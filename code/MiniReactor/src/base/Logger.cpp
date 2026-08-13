#include "base/Logger.h"

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace minireactor {

namespace {

std::mutex& configMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unique_ptr<LoggerConfig>& pendingConfig() {
    static std::unique_ptr<LoggerConfig> config;
    return config;
}

bool& loggerCreated() {
    static bool created = false;
    return created;
}

}  // namespace

LoggerConfig Logger::defaultConfig() {
    LoggerConfig config;
    config.file_path = "logs/minireactor.log";
    config.min_level = LogLevel::Debug;
    config.flush_interval = std::chrono::milliseconds(1000);
    return config;
}

void Logger::init(LoggerConfig config) {
    std::lock_guard<std::mutex> lock(configMutex());
    if (loggerCreated()) {
        throw std::logic_error("Logger::init must be called before the first log");
    }
    pendingConfig() = std::make_unique<LoggerConfig>(std::move(config));
}

Logger& Logger::instance() {
    static Logger* logger = [] {
        std::unique_ptr<LoggerConfig> config;
        {
            std::lock_guard<std::mutex> lock(configMutex());
            if (pendingConfig() != nullptr) {
                config = std::move(pendingConfig());
            }
            loggerCreated() = true;
        }
        auto* instance = new Logger();
        instance->async_ = std::make_unique<AsyncLogger>(config != nullptr ? *config
                                                                           : defaultConfig());
        instance->async_->start();
        return instance;
    }();
    return *logger;
}

void Logger::log(LogLevel level, std::string_view message, const char* file, int line,
                 const char* function) {
    async_->log(level, message, file, line, function);
}

AsyncLogger& Logger::asyncLogger() noexcept { return *async_; }

}  // namespace minireactor
