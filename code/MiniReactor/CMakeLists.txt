cmake_minimum_required(VERSION 3.16)
project(MiniReactor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(minireactor
    src/net/Acceptor.cpp
    src/net/Buffer.cpp
    src/net/Channel.cpp
    src/net/EpollPoller.cpp
    src/net/EventLoop.cpp
    src/net/Socket.cpp
    src/net/TcpConnection.cpp
    src/net/TcpServer.cpp
)

target_include_directories(minireactor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(minireactor PRIVATE -Wall -Wextra -Wpedantic)

add_executable(echo_server example/EchoServer.cpp)
target_link_libraries(echo_server PRIVATE minireactor)

enable_testing()
add_executable(buffer_test test/BufferTest.cpp)
target_link_libraries(buffer_test PRIVATE minireactor)
add_test(NAME buffer_test COMMAND buffer_test)
