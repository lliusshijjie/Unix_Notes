cmake_minimum_required(VERSION 3.16)

set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)

project(TimerSchedulerPhase1 LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "Must use GCC, current: ${CMAKE_CXX_COMPILER_ID}")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(test_phase1
    tests/test_phase1.cpp
    TimerScheduler.cpp
    ../ThreadPool/ThreadPool.cpp
)
target_include_directories(test_phase1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_phase1 PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(test_phase1 PRIVATE pthread)

add_executable(test_phase2
    tests/test_phase2.cpp
    TimerScheduler.cpp
    ../ThreadPool/ThreadPool.cpp
)
target_include_directories(test_phase2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_phase2 PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(test_phase2 PRIVATE pthread)

add_executable(test_phase3
    tests/test_phase3.cpp
    TimerScheduler.cpp
    ../ThreadPool/ThreadPool.cpp
)
target_include_directories(test_phase3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_phase3 PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(test_phase3 PRIVATE pthread)
