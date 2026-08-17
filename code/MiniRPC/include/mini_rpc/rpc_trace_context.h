#pragma once

#include <cstdint>
#include <string>

namespace minirpc {

struct RpcTraceContext {
    std::string trace_id;
    std::uint64_t request_id{0};
    std::string service_name;
    std::string method_name;
};

}  // namespace minirpc
