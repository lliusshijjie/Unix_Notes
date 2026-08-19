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

using namespace std::chrono_literals;

namespace {

constexpr std::uint16_t kPortA = 39196;
constexpr std::uint16_t kPortB = 39197;

void registerServerA(minirpc::RpcServer& server) {
    assert(server.registerMethod(
        "TestService", "Echo",
        [](const minirpc::RpcRequest&, minirpc::RpcResponse&) {
            std::this_thread::sleep_for(3s);
        }));
    assert(server.registerMethod(
        "TestService", "Slow",
        [](const minirpc::RpcRequest&, minirpc::RpcResponse&) {
            std::this_thread::sleep_for(3s);
        }));
}

void registerServerB(minirpc::RpcServer& server) {
    assert(server.registerMethod(
        "TestService", "Echo",
        [](const minirpc::RpcRequest& request, minirpc::RpcResponse& response) {
            response.payload = request.payload + "@B";
        }));
    assert(server.registerMethod(
        "TestService", "Slow",
        [](const minirpc::RpcRequest&, minirpc::RpcResponse&) {
            std::this_thread::sleep_for(3s);
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
        registerServerA(serverA);
        registerServerB(serverB);
        serverA.start();
        serverB.start();
        serverReady.set_value(&loop);
        loop.loop();
    });
    assert(serverReadyFuture.wait_for(2s) == std::future_status::ready);
    minireactor::EventLoop* serverLoop = serverReadyFuture.get();

    auto discovery = std::make_shared<minirpc::ServiceDiscovery>();
    discovery->registerEndpoint("TestService", {"127.0.0.1", kPortA});
    discovery->registerEndpoint("TestService", {"127.0.0.1", kPortB});

    {
        minirpc::RpcClient client("TestService", discovery);
        client.setMaxRetries(1);
        assert(client.connect(2s));

        const auto started = std::chrono::steady_clock::now();
        const minirpc::RpcResponse response =
            client.call({0, "TestService", "Echo", "value"}, 2s);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        assert(response.error_code == 0);
        assert(response.payload == "value@B");
        assert(elapsed >= 700ms);
        assert(elapsed < 1900ms);

        client.disconnect();
    }

    {
        minirpc::RpcClient client("TestService", discovery);
        client.setMaxRetries(1);
        assert(client.connect(2s));

        const auto started = std::chrono::steady_clock::now();
        const minirpc::RpcResponse response =
            client.call({0, "TestService", "Slow", ""}, 1s);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        assert(response.error_code == static_cast<int>(minirpc::RpcErrorCode::Timeout));
        assert(elapsed >= 800ms);
        assert(elapsed < 1500ms);

        client.disconnect();
    }

    {
        minirpc::RpcClient client("TestService", discovery);
        client.setMaxRetries(0);
        assert(client.connect(2s));

        const auto started = std::chrono::steady_clock::now();
        const minirpc::RpcResponse response =
            client.call({0, "TestService", "Slow", ""}, 500ms);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        assert(response.error_code == static_cast<int>(minirpc::RpcErrorCode::Timeout));
        assert(elapsed >= 400ms);
        assert(elapsed < 900ms);

        client.disconnect();
    }

    serverLoop->quit();
    serverThread.join();
}
