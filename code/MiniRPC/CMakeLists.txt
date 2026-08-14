cmake_minimum_required(VERSION 3.20)

project(MiniRPC LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MINIRPC_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MINIRPC_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
if(MINIRPC_ENABLE_ASAN AND MINIRPC_ENABLE_TSAN)
    message(FATAL_ERROR "ASan and TSan must use separate build directories")
endif()
if(MINIRPC_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(MINIRPC_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

set(MINIREACTOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MINIREACTOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASYNC_LOGGER_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(ASYNC_LOGGER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(../MiniReactor wheels/minireactor)

add_library(minirpc
    src/codec.cpp
    src/rpc_channel.cpp
    src/rpc_client.cpp
    src/rpc_controller.cpp
    src/rpc_server.cpp
    src/service_registry.cpp
)
target_include_directories(minirpc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(minirpc PUBLIC minireactor)
target_compile_options(minirpc PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()

add_executable(codec_test tests/codec_test.cpp)
target_link_libraries(codec_test PRIVATE minirpc)
add_test(NAME codec_test COMMAND codec_test)

add_executable(registry_test tests/registry_test.cpp)
target_link_libraries(registry_test PRIVATE minirpc)
add_test(NAME registry_test COMMAND registry_test)

add_executable(rpc_server_test tests/rpc_server_test.cpp)
target_link_libraries(rpc_server_test PRIVATE minirpc)
add_test(NAME rpc_server_test COMMAND rpc_server_test)

add_executable(rpc_client_test tests/rpc_client_test.cpp)
target_link_libraries(rpc_client_test PRIVATE minirpc)
add_test(NAME rpc_client_test COMMAND rpc_client_test)

add_executable(rpc_channel_test tests/rpc_channel_test.cpp)
target_link_libraries(rpc_channel_test PRIVATE minirpc)
add_test(NAME rpc_channel_test COMMAND rpc_channel_test)

add_executable(calculator_server examples/calculator_server.cpp)
target_link_libraries(calculator_server PRIVATE minirpc)

add_executable(calculator_client examples/calculator_client.cpp)
target_link_libraries(calculator_client PRIVATE minirpc)
