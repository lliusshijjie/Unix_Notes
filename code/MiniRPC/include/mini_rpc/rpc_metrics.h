#pragma once

#include <cstdint>

namespace minirpc {

struct RpcMetricsSnapshot {
    std::uint64_t client_total_calls{0};
    std::uint64_t client_success_calls{0};
    std::uint64_t client_failed_calls{0};
    std::uint64_t client_timeout_calls{0};
    std::uint64_t client_cancelled_calls{0};
    std::uint64_t client_reconnect_count{0};
    std::uint64_t client_pending_calls{0};

    std::uint64_t server_total_requests{0};
    std::uint64_t server_success_responses{0};
    std::uint64_t server_failed_responses{0};
    std::uint64_t server_timeout_responses{0};
    std::uint64_t server_service_not_found{0};
    std::uint64_t server_method_not_found{0};
    std::uint64_t server_error_responses{0};
    std::uint64_t server_queue_rejected{0};
    std::uint64_t server_current_queue_size{0};
};

}  // namespace minirpc
