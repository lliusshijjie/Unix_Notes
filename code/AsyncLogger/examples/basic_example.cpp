#include "async_logger/async_logger.h"

#include <chrono>
#include <iostream>

int main() {
    LoggerConfig config;
    config.file_path = "logs/server.log";
    config.min_level = LogLevel::Debug;
    config.flush_interval = std::chrono::milliseconds(500);
    config.max_file_size = 10 * 1024 * 1024;
    config.max_backup_files = 3;

    AsyncLogger logger(config);
    logger.start();

    LOG_INFO(logger, "server started");
    LOG_ERROR(logger, "example error message");
    logger.flush();
    logger.stop();

    return logger.has_error() ? 1 : 0;
}
