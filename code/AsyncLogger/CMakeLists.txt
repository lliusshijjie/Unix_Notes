cmake_minimum_required(VERSION 3.20)

project(MyAsyncLogger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(async_logger
    src/async_logger.cpp
    src/log_level.cpp
)
target_include_directories(async_logger PUBLIC include)
target_link_libraries(async_logger PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(async_logger PRIVATE /W4 /permissive-)
else()
    target_compile_options(async_logger PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_executable(async_logger_example examples/basic_example.cpp)
target_link_libraries(async_logger_example PRIVATE async_logger)

add_executable(async_logger_test tests/async_logger_test.cpp)
target_link_libraries(async_logger_test PRIVATE async_logger)

add_executable(async_logger_compile_filter_test tests/compile_filter_test.cpp)
target_link_libraries(async_logger_compile_filter_test PRIVATE async_logger)
target_compile_definitions(async_logger_compile_filter_test PRIVATE ASYNC_LOG_COMPILED_MIN_LEVEL=3)

enable_testing()
add_test(NAME async_logger_test COMMAND async_logger_test)
add_test(NAME async_logger_compile_filter_test COMMAND async_logger_compile_filter_test)
