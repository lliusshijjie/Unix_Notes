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

namespace {

constexpr std::uint16_t kPort = 39190;

}  // namespace

int main() {
    using namespace std::chrono_literals;

    std::promise<minireactor::EventLoop*> serverReady;
    auto serverReadyFuture = serverReady.get_future();
    std::thread serverThread([&serverReady] {
        minireactor::EventLoop loop;
        minirpc::RpcServer server(
            &loop, minireactor::InetAddress("127.0.0.1", kPort), 1, 2, 16);
        assert(server.registerMethod(
            "TestService", "Slow",
            [](const minirpc::RpcRequest& request, minirpc::RpcResponse& response) {
                std::this_thread::sleep_for(2s);
                response.payload = request.payload;
            }));
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

    const auto started = std::chrono::steady_clock::now();
    std::future<minirpc::RpcResponse> future =
        client.asyncCall({0, "TestService", "Slow", "late"}, 500ms);
    const minirpc::RpcResponse timedOut = future.get();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    assert(timedOut.error_code == static_cast<int>(minirpc::RpcErrorCode::Timeout));
    assert(elapsed >= 400ms);
    assert(elapsed < 1500ms);

    std::this_thread::sleep_for(2s);
    const minirpc::RpcResponse afterTimeout =
        client.call({0, "TestService", "Echo", "alive"}, 2s);
    assert(afterTimeout.error_code == 0);
    assert(afterTimeout.payload == "alive");

    client.disconnect();
    serverLoop->quit();
    serverThread.join();
}
