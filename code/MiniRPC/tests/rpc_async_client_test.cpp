#include "mini_rpc/protocol.h"
#include "mini_rpc/rpc_client.h"
#include "mini_rpc/rpc_server.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::uint16_t kPort = 39189;
constexpr int kRequestCount = 100;

}  // namespace

int main() {
    using namespace std::chrono_literals;

    std::promise<minireactor::EventLoop*> serverReady;
    auto serverReadyFuture = serverReady.get_future();
    std::thread serverThread([&serverReady] {
        minireactor::EventLoop loop;
        minirpc::RpcServer server(
            &loop, minireactor::InetAddress("127.0.0.1", kPort), 1, 4, 256);
        assert(server.registerMethod(
            "TestService", "Echo",
            [](const minirpc::RpcRequest& request, minirpc::RpcResponse& response) {
                response.payload = request.payload;
            }));
        server.start();
        serverReady.set_value(&loop);
        loop.loop();
    });

    assert(serverReadyFuture.wait_for(2s) == std::future_status::ready);
    minireactor::EventLoop* serverLoop = serverReadyFuture.get();

    minirpc::RpcClient client("127.0.0.1", kPort);
    assert(client.connect(2s));

    std::vector<std::future<minirpc::RpcResponse>> futures;
    futures.reserve(kRequestCount);
    for (int index = 0; index < kRequestCount; ++index) {
        futures.push_back(client.asyncCall(
            {0, "TestService", "Echo", "value-" + std::to_string(index)}));
    }

    std::unordered_set<std::uint64_t> requestIds;
    for (int index = 0; index < kRequestCount; ++index) {
        const minirpc::RpcResponse response = futures[index].get();
        assert(response.error_code == 0);
        assert(response.payload == "value-" + std::to_string(index));
        assert(response.request_id != 0);
        assert(requestIds.insert(response.request_id).second);
    }
    assert(requestIds.size() == static_cast<std::size_t>(kRequestCount));

    client.disconnect();
    serverLoop->quit();
    serverThread.join();
}
