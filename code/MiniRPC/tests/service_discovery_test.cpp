#include "mini_rpc/rpc_client.h"
#include "mini_rpc/service_discovery.h"
#include "mini_rpc/protocol.h"

#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

int main() {
    using namespace std::chrono_literals;

    minirpc::ServiceDiscovery discovery;
    discovery.registerEndpoint("CalculatorService", {"10.0.0.1", 8000});
    discovery.registerEndpoint("CalculatorService", {"10.0.0.2", 8000});
    discovery.registerEndpoint("CalculatorService", {"10.0.0.3", 8000});
    discovery.registerEndpoint("CalculatorService", {"10.0.0.1", 8000});

    const std::vector<minirpc::Endpoint> endpoints = discovery.discover("CalculatorService");
    assert(endpoints.size() == 3);
    assert((endpoints[0] == minirpc::Endpoint{"10.0.0.1", 8000}));
    assert((endpoints[1] == minirpc::Endpoint{"10.0.0.2", 8000}));
    assert((endpoints[2] == minirpc::Endpoint{"10.0.0.3", 8000}));

    discovery.unregisterEndpoint("CalculatorService", {"10.0.0.2", 8000});
    const std::vector<minirpc::Endpoint> remaining = discovery.discover("CalculatorService");
    assert(remaining.size() == 2);
    assert((remaining[0] == minirpc::Endpoint{"10.0.0.1", 8000}));
    assert((remaining[1] == minirpc::Endpoint{"10.0.0.3", 8000}));

    assert(discovery.discover("MissingService").empty());

    discovery.unregisterEndpoint("CalculatorService", {"10.0.0.1", 8000});
    discovery.unregisterEndpoint("CalculatorService", {"10.0.0.3", 8000});
    assert(discovery.discover("CalculatorService").empty());

    auto emptyDiscovery = std::make_shared<minirpc::ServiceDiscovery>();
    minirpc::RpcClient unavailable("CalculatorService", emptyDiscovery);
    assert(!unavailable.connect(200ms));
    std::future<minirpc::RpcResponse> future = unavailable.asyncCall(
        {0, "CalculatorService", "Add", ""}, 2s);
    assert(future.wait_for(100ms) == std::future_status::ready);
    const minirpc::RpcResponse response = future.get();
    assert(response.error_code == static_cast<int>(minirpc::RpcErrorCode::ServiceUnavailable));
}
