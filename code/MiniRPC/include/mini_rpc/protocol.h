#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace minirpc {

constexpr std::uint32_t kRpcMagic = 0x4D525043;
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::size_t kRpcHeaderSize = 24;
constexpr std::size_t kMaxMessageSize = 16 * 1024 * 1024;

enum class MessageType : std::uint16_t {
    Request = 1,
    Response = 2
};

enum class RpcErrorCode : int {
    Ok = 0,
    NetworkError = 1001,
    ProtocolError = 1002,
    Timeout = 1003,
    Cancelled = 1004,
    ClientStopping = 1005,
    QueueingTimeout = 1006,
    RetryExhausted = 1007,
    ServiceNotFound = 2001,
    MethodNotFound = 2002,
    ServerError = 3001,
    SerializationError = 4001,
    DeserializationError = 4002
};

struct RpcHeader {
    std::uint32_t magic{kRpcMagic};
    std::uint16_t version{kProtocolVersion};
    std::uint16_t message_type{0};
    std::uint64_t request_id{0};
    std::uint32_t metadata_length{0};
    std::uint32_t body_length{0};
};

struct RpcRequest {
    std::uint64_t request_id{0};
    std::string service_name;
    std::string method_name;
    std::string payload;
    std::uint64_t timeout_ms{0};
    std::string trace_id;
    std::string serializer{"raw"};
    std::uint32_t attempt{0};
};

struct RpcResponse {
    std::uint64_t request_id{0};
    int error_code{0};
    std::string error_message;
    std::string payload;
    std::string trace_id;
    std::uint64_t server_cost_us{0};
    std::string serializer{"raw"};
};

}  // namespace minirpc
