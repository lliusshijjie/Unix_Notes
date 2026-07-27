cmake_minimum_required(VERSION 3.16)

set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)

project(ThreadPoolPhase1Test LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "Must use GCC, current: ${CMAKE_CXX_COMPILER_ID}")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(test_phase1
    test_phase1.cpp
    ../ThreadPool.cpp
)
target_include_directories(test_phase1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(bench_phase31
    bench_phase31.cpp
    ../ThreadPool.cpp
)
target_include_directories(bench_phase31 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
