#include "mini_rpc/service_registry.h"

#include <atomic>
#include <cassert>
#include <string>
#include <thread>
#include <vector>

int main() {
    minirpc::ServiceRegistry registry;
    const minirpc::MethodHandler handler =
        [](const minirpc::RpcRequest& request, minirpc::RpcResponse& response) {
            response.payload = request.payload;
        };

    assert(registry.registerMethod("CalculatorService", "Add", handler));
    assert(!registry.registerMethod("CalculatorService", "Add", handler));

    const minirpc::LookupResult missingService = registry.findMethod("Missing", "Add");
    assert(missingService.status == minirpc::LookupStatus::ServiceNotFound);
    assert(!missingService.handler);

    const minirpc::LookupResult missingMethod =
        registry.findMethod("CalculatorService", "Missing");
    assert(missingMethod.status == minirpc::LookupStatus::MethodNotFound);
    assert(!missingMethod.handler);

    const minirpc::LookupResult found = registry.findMethod("CalculatorService", "Add");
    assert(found.status == minirpc::LookupStatus::Found);
    assert(static_cast<bool>(found.handler));

    std::atomic<int> completed{0};
    std::vector<std::thread> readers;
    for (int threadIndex = 0; threadIndex < 8; ++threadIndex) {
        readers.emplace_back([&registry, &completed] {
            for (int index = 0; index < 1000; ++index) {
                const minirpc::LookupResult result =
                    registry.findMethod("CalculatorService", "Add");
                assert(result.status == minirpc::LookupStatus::Found);
                minirpc::RpcRequest request;
                request.payload = std::to_string(index);
                minirpc::RpcResponse response;
                result.handler(request, response);
                assert(response.payload == request.payload);
                completed.fetch_add(1);
            }
        });
    }
    for (std::thread& reader : readers) {
        reader.join();
    }
    assert(completed.load() == 8000);
}
