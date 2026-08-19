#include "mini_rpc/endpoint.h"
#include "mini_rpc/round_robin_load_balancer.h"

#include <cassert>
#include <stdexcept>
#include <vector>

int main() {
    minirpc::RoundRobinLoadBalancer balancer;
    const std::vector<minirpc::Endpoint> endpoints = {
        {"10.0.0.1", 8001}, {"10.0.0.2", 8002}, {"10.0.0.3", 8003}};
    assert((balancer.select(endpoints) == minirpc::Endpoint{"10.0.0.1", 8001}));
    assert((balancer.select(endpoints) == minirpc::Endpoint{"10.0.0.2", 8002}));
    assert((balancer.select(endpoints) == minirpc::Endpoint{"10.0.0.3", 8003}));
    assert((balancer.select(endpoints) == minirpc::Endpoint{"10.0.0.1", 8001}));

    bool emptyRejected = false;
    try {
        balancer.select({});
    } catch (const std::invalid_argument&) {
        emptyRejected = true;
    }
    assert(emptyRejected);
}
