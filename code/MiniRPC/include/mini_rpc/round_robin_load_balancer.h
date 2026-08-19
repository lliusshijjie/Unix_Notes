#pragma once

#include "mini_rpc/load_balancer.h"

#include <atomic>
#include <cstddef>

namespace minirpc {

class RoundRobinLoadBalancer : public LoadBalancer {
public:
    Endpoint select(const std::vector<Endpoint>& endpoints) override;

private:
    std::atomic<std::size_t> index_{0};
};

}  // namespace minirpc
