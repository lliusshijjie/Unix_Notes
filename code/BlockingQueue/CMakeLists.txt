cmake_minimum_required(VERSION 3.16)

set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)

project(BlockingQueuePhase1 LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "Must use GCC, current: ${CMAKE_CXX_COMPILER_ID}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(blocking_queue_test
    tests/blocking_queue_test.cpp
)

target_include_directories(blocking_queue_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(blocking_queue_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)
