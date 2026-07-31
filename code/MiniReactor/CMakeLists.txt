cmake_minimum_required(VERSION 3.16)
project(MiniReactor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(minireactor
    src/net/Acceptor.cpp
    src/net/Buffer.cpp
    src/net/Channel.cpp
    src/net/EpollPoller.cpp
    src/net/EventLoop.cpp
    src/net/EventLoopThread.cpp
    src/net/EventLoopThreadPool.cpp
    src/net/Socket.cpp
    src/net/TcpConnection.cpp
    src/net/TcpServer.cpp
)

target_include_directories(minireactor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(minireactor PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(minireactor PUBLIC Threads::Threads)

add_executable(echo_server example/EchoServer.cpp)
target_link_libraries(echo_server PRIVATE minireactor)

enable_testing()
add_executable(buffer_test test/BufferTest.cpp)
target_link_libraries(buffer_test PRIVATE minireactor)
add_test(NAME buffer_test COMMAND buffer_test)

add_executable(event_loop_thread_test test/EventLoopThreadTest.cpp)
target_link_libraries(event_loop_thread_test PRIVATE minireactor)
add_test(NAME event_loop_thread_test COMMAND event_loop_thread_test)

add_executable(echo_server_test test/EchoServerTest.cpp)
target_link_libraries(echo_server_test PRIVATE minireactor)
add_test(NAME echo_server_test COMMAND echo_server_test)
