#include "mini_rpc/protocol.h"
#include "mini_rpc/rpc_client.h"
#include "mini_rpc/rpc_server.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint16_t kPort = 39186;

}  // namespace

int main() {
    using namespace std::chrono_literals;

    std::promise<minireactor::EventLoop*> serverReady;
    auto serverReadyFuture = serverReady.get_future();
    std::thread serverThread([&serverReady] {
        minireactor::EventLoop loop;
        minirpc::RpcServer server(
            &loop, minireactor::InetAddress("127.0.0.1", kPort), 1, 4, 32);
        assert(server.registerMethod(
            "TestService", "DelayEcho",
            [](const minirpc::RpcRequest& request, minirpc::RpcResponse& response) {
                std::istringstream input(request.payload);
                int delayMilliseconds = 0;
                std::string value;
                input >> delayMilliseconds >> value;
                assert(input);
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(delayMilliseconds));
                response.payload = value;
            }));
        server.start();
        serverReady.set_value(&loop);
        loop.loop();
    });

    assert(serverReadyFuture.wait_for(2s) == std::future_status::ready);
    minireactor::EventLoop* serverLoop = serverReadyFuture.get();

    {
        minirpc::RpcClient client("127.0.0.1", kPort);
        assert(client.connect(2s));
        assert(client.connected());

        const minirpc::RpcResponse first = client.call(
            {0, "TestService", "DelayEcho", "0 first"}, 2s);
        assert(first.error_code == 0);
        assert(first.payload == "first");
        assert(first.request_id != 0);

        std::vector<std::future<minirpc::RpcResponse>> calls;
        for (int index = 0; index < 8; ++index) {
            calls.push_back(std::async(std::launch::async, [&client, index] {
                const int delay = (7 - index) * 15;
                return client.call(
                    {0, "TestService", "DelayEcho",
                     std::to_string(delay) + " value-" + std::to_string(index)},
                    std::chrono::seconds(2));
            }));
        }
        for (int index = 0; index < 8; ++index) {
            const minirpc::RpcResponse response = calls[index].get();
            assert(response.error_code == 0);
            assert(response.payload == "value-" + std::to_string(index));
        }

        const minirpc::RpcResponse timedOut = client.call(
            {0, "TestService", "DelayEcho", "300 late"}, 50ms);
        assert(timedOut.error_code == static_cast<int>(minirpc::RpcErrorCode::Timeout));
        std::this_thread::sleep_for(350ms);

        const minirpc::RpcResponse afterTimeout = client.call(
            {0, "TestService", "DelayEcho", "0 after-timeout"}, 2s);
        assert(afterTimeout.error_code == 0);
        assert(afterTimeout.payload == "after-timeout");

        auto interrupted = std::async(std::launch::async, [&client] {
            return client.call(
                {0, "TestService", "DelayEcho", "300 interrupted"},
                std::chrono::seconds(2));
        });
        std::this_thread::sleep_for(50ms);
        client.disconnect();
        assert(interrupted.wait_for(2s) == std::future_status::ready);
        assert(interrupted.get().error_code ==
               static_cast<int>(minirpc::RpcErrorCode::NetworkError));
        assert(!client.connected());
    }

    serverLoop->quit();
    serverThread.join();

    minirpc::RpcClient refused("127.0.0.1", kPort);
    assert(!refused.connect(2s));
    assert(!refused.connected());
}
