#include "async_logger/async_logger.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main() {
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       ("async_logger_compile_filter_" +
                                        std::to_string(std::rand()) + ".log");
    LoggerConfig config;
    config.file_path = path;
    config.min_level = LogLevel::Trace;
    config.flush_interval = std::chrono::milliseconds(10);

    int evaluated = 0;
    AsyncLogger logger(config);
    logger.start();
    LOG_INFO(logger, std::to_string(++evaluated));
    LOG_WARN(logger, std::to_string(++evaluated));
    logger.stop();

    std::ifstream input(path);
    const std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    std::error_code error;
    std::filesystem::remove(path, error);
    if (evaluated == 1 && content.find("[WARN]") != std::string::npos &&
        content.find("[INFO]") == std::string::npos) {
        return 0;
    }
    std::cerr << "compile-time filtering test failed\n";
    return 1;
}
