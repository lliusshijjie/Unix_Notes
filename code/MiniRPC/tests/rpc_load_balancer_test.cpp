#include "mini_rpc/protocol.h"
#include "mini_rpc/rpc_client.h"
#include "mini_rpc/rpc_server.h"
#include "mini_rpc/service_discovery.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint16_t kPortA = 39191;
constexpr std::uint16_t kPortB = 39192;
constexpr std::uint16_t kPortC = 39193;

void registerId(minirpc::RpcServer& server, std::string serverId) {
    assert(server.registerMethod(
        "TestService", "Id",
        [serverId = std::move(serverId)](const minirpc::RpcRequest&,
                                         minirpc::RpcResponse& response) {
            response.payload = serverId;
        }));
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    std::promise<minireactor::EventLoop*> serverReady;
    auto serverReadyFuture = serverReady.get_future();
    std::thread serverThread([&serverReady] {
        minireactor::EventLoop loop;
        minirpc::RpcServer serverA(
            &loop, minireactor::InetAddress("127.0.0.1", kPortA), 1, 2, 16);
        minirpc::RpcServer serverB(
            &loop, minireactor::InetAddress("127.0.0.1", kPortB), 1, 2, 16);
        minirpc::RpcServer serverC(
            &loop, minireactor::InetAddress("127.0.0.1", kPortC), 1, 2, 16);
        registerId(serverA, "A");
        registerId(serverB, "B");
        registerId(serverC, "C");
        serverA.start();
        serverB.start();
        serverC.start();
        serverReady.set_value(&loop);
        loop.loop();
    });

    assert(serverReadyFuture.wait_for(2s) == std::future_status::ready);
    minireactor::EventLoop* serverLoop = serverReadyFuture.get();

    auto discovery = std::make_shared<minirpc::ServiceDiscovery>();
    discovery->registerEndpoint("TestService", {"127.0.0.1", kPortA});
    discovery->registerEndpoint("TestService", {"127.0.0.1", kPortB});
    discovery->registerEndpoint("TestService", {"127.0.0.1", kPortC});
    assert(discovery->discover("TestService").size() == 3);

    minirpc::RpcClient client("TestService", discovery);
    assert(client.connect(2s));

    const std::vector<std::string> expected = {"A", "B", "C", "A"};
    for (const std::string& serverId : expected) {
        const minirpc::RpcResponse response =
            client.call({0, "TestService", "Id", ""}, 2s);
        assert(response.error_code == 0);
        assert(response.payload == serverId);
    }

    client.disconnect();
    serverLoop->quit();
    serverThread.join();
}
