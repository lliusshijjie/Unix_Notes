#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace minirpc {

struct RpcCallOptions {
    std::chrono::milliseconds timeout{3000};
    bool retry_enabled{false};
    std::size_t max_retries{0};
    bool fail_fast{true};
    std::string trace_id;
};

}  // namespace minirpc
