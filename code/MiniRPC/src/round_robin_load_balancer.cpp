#include "mini_rpc/round_robin_load_balancer.h"

#include <stdexcept>

namespace minirpc {

Endpoint RoundRobinLoadBalancer::select(const std::vector<Endpoint>& endpoints) {
    if (endpoints.empty()) {
        throw std::invalid_argument("no available endpoint");
    }
    const std::size_t index = index_.fetch_add(1, std::memory_order_relaxed);
    return endpoints[index % endpoints.size()];
}

}  // namespace minirpc
