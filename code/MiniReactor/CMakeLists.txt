cmake_minimum_required(VERSION 3.16)

project(MiniReactor LANGUAGES CXX)

option(MINIREACTOR_BUILD_EXAMPLES "Build MiniReactor examples" ON)
option(MINIREACTOR_BUILD_TESTS "Build MiniReactor tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

enable_testing()

# ---- 轮子工程集成 ----
# AsyncLogger：自带库 target async_logger（含其自身的单元测试）
add_subdirectory(../AsyncLogger wheels/async_logger)

# ThreadPool / TimerScheduler / BlockingQueue：以源码 + 头文件路径方式接入
set(WHEELS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_library(minireactor
    src/base/Logger.cpp
    src/base/TimerManager.cpp
    src/net/Acceptor.cpp
    src/net/Buffer.cpp
    src/net/Channel.cpp
    src/net/Connector.cpp
    src/net/EpollPoller.cpp
    src/net/EventLoop.cpp
    src/net/EventLoopThread.cpp
    src/net/EventLoopThreadPool.cpp
    src/net/InetAddress.cpp
    src/net/Socket.cpp
    src/net/TcpClient.cpp
    src/net/TcpConnection.cpp
    src/net/TcpServer.cpp
    ${WHEELS_DIR}/ThreadPool/ThreadPool.cpp
    ${WHEELS_DIR}/TimerScheduler/TimerScheduler.cpp
)

target_include_directories(minireactor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${WHEELS_DIR}/ThreadPool
    ${WHEELS_DIR}/TimerScheduler
    ${WHEELS_DIR}/BlockingQueue/include
)
target_compile_options(minireactor PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(minireactor PUBLIC Threads::Threads async_logger)

if(MINIREACTOR_BUILD_EXAMPLES)
add_executable(echo_server example/EchoServer.cpp)
target_link_libraries(echo_server PRIVATE minireactor)

add_executable(idle_echo_server example/IdleEchoServer.cpp)
target_link_libraries(idle_echo_server PRIVATE minireactor)

add_executable(compute_server example/ComputeServer.cpp)
target_link_libraries(compute_server PRIVATE minireactor)
# BlockingQueue 轮子使用 std::span，要求 C++20（框架库本身保持 C++17）
set_target_properties(compute_server PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
endif()

if(MINIREACTOR_BUILD_TESTS)
add_executable(buffer_test test/BufferTest.cpp)
target_link_libraries(buffer_test PRIVATE minireactor)
add_test(NAME buffer_test COMMAND buffer_test)

add_executable(event_loop_thread_test test/EventLoopThreadTest.cpp)
target_link_libraries(event_loop_thread_test PRIVATE minireactor)
add_test(NAME event_loop_thread_test COMMAND event_loop_thread_test)

add_executable(echo_server_test test/EchoServerTest.cpp)
target_link_libraries(echo_server_test PRIVATE minireactor)
add_test(NAME echo_server_test COMMAND echo_server_test)

add_executable(timer_manager_test test/TimerManagerTest.cpp)
target_link_libraries(timer_manager_test PRIVATE minireactor)
add_test(NAME timer_manager_test COMMAND timer_manager_test)

add_executable(idle_timeout_test test/IdleTimeoutTest.cpp)
target_link_libraries(idle_timeout_test PRIVATE minireactor)
add_test(NAME idle_timeout_test COMMAND idle_timeout_test)

add_executable(logger_test test/LoggerTest.cpp)
target_link_libraries(logger_test PRIVATE minireactor)
add_test(NAME logger_test COMMAND logger_test)

add_executable(inet_address_test test/InetAddressTest.cpp)
target_link_libraries(inet_address_test PRIVATE minireactor)
add_test(NAME inet_address_test COMMAND inet_address_test)

add_executable(connector_test test/ConnectorTest.cpp)
target_link_libraries(connector_test PRIVATE minireactor)
add_test(NAME connector_test COMMAND connector_test)

add_executable(tcp_client_test test/TcpClientTest.cpp)
target_link_libraries(tcp_client_test PRIVATE minireactor)
add_test(NAME tcp_client_test COMMAND tcp_client_test)
endif()
