#pragma once

#include "mini_rpc/endpoint.h"

#include <vector>

namespace minirpc {

class LoadBalancer {
public:
    virtual ~LoadBalancer() = default;
    virtual Endpoint select(const std::vector<Endpoint>& endpoints) = 0;
};

}  // namespace minirpc
