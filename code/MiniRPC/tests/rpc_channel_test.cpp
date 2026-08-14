#include "mini_rpc/protocol.h"
#include "mini_rpc/rpc_channel.h"
#include "mini_rpc/rpc_client.h"
#include "mini_rpc/rpc_controller.h"
#include "mini_rpc/rpc_server.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr std::uint16_t kPort = 39187;

}  // namespace

int main() {
    using namespace std::chrono_literals;

    minirpc::RpcController state;
    assert(!state.failed());
    assert(state.errorCode() == 0);
    assert(state.errorText().empty());
    assert(state.timeout() == 3000ms);
    state.setTimeout(250ms);
    state.setFailed(123, "failed");
    assert(state.failed());
    assert(state.errorCode() == 123);
    assert(state.errorText() == "failed");
    state.reset();
    assert(!state.failed());
    assert(state.errorCode() == 0);
    assert(state.errorText().empty());
    assert(state.timeout() == 250ms);

    bool rejectedTimeout = false;
    try {
        state.setTimeout(0ms);
    } catch (const std::invalid_argument&) {
        rejectedTimeout = true;
    }
    assert(rejectedTimeout);

    std::promise<minireactor::EventLoop*> serverReady;
    auto serverReadyFuture = serverReady.get_future();
    std::thread serverThread([&serverReady] {
        minireactor::EventLoop loop;
        minirpc::RpcServer server(
            &loop, minireactor::InetAddress("127.0.0.1", kPort), 1, 2, 16);
        assert(server.registerMethod(
            "EchoService", "Echo",
            [](const minirpc::RpcRequest& request, minirpc::RpcResponse& response) {
                response.payload = request.payload;
            }));
        assert(server.registerMethod(
            "EchoService", "Slow",
            [](const minirpc::RpcRequest& request, minirpc::RpcResponse& response) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                response.payload = request.payload;
            }));
        server.start();
        serverReady.set_value(&loop);
        loop.loop();
    });

    assert(serverReadyFuture.wait_for(2s) == std::future_status::ready);
    minireactor::EventLoop* serverLoop = serverReadyFuture.get();

    auto client = std::make_shared<minirpc::RpcClient>("127.0.0.1", kPort);
    assert(client->connect(2s));
    minirpc::RpcChannel channel(client);

    minirpc::RpcController controller;
    std::string responsePayload;
    assert(channel.callMethod("EchoService", "Echo", "hello", responsePayload,
                              controller));
    assert(responsePayload == "hello");
    assert(!controller.failed());

    assert(!channel.callMethod("UnknownService", "Echo", "hello", responsePayload,
                               controller));
    assert(controller.failed());
    assert(controller.errorCode() ==
           static_cast<int>(minirpc::RpcErrorCode::ServiceNotFound));

    controller.setTimeout(20ms);
    assert(!channel.callMethod("EchoService", "Slow", "late", responsePayload,
                               controller));
    assert(controller.errorCode() == static_cast<int>(minirpc::RpcErrorCode::Timeout));

    client->disconnect();
    client.reset();
    serverLoop->quit();
    serverThread.join();
}
