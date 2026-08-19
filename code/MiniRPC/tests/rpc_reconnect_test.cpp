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

constexpr std::uint16_t kPort = 39195;

void registerEcho(minirpc::RpcServer& server) {
    assert(server.registerMethod(
        "TestService", "Echo",
        [](const minirpc::RpcRequest& request, minirpc::RpcResponse& response) {
            response.payload = request.payload;
        }));
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    std::promise<minireactor::EventLoop*> server1Ready;
    auto server1ReadyFuture = server1Ready.get_future();
    std::thread server1Thread([&server1Ready] {
        minireactor::EventLoop loop;
        minirpc::RpcServer server(
            &loop, minireactor::InetAddress("127.0.0.1", kPort), 1, 2, 16);
        registerEcho(server);
        server.start();
        server1Ready.set_value(&loop);
        loop.loop();
    });
    assert(server1ReadyFuture.wait_for(2s) == std::future_status::ready);
    minireactor::EventLoop* server1Loop = server1ReadyFuture.get();

    {
        minirpc::RpcClient client("127.0.0.1", kPort);
        client.setReconnectPolicy(100ms, 500ms);
        assert(client.connect(2s));

        const minirpc::RpcResponse before =
            client.call({0, "TestService", "Echo", "hello"}, 2s);
        assert(before.error_code == 0);
        assert(before.payload == "hello");

        server1Loop->quit();
        server1Thread.join();
        std::this_thread::sleep_for(600ms);

        std::promise<minireactor::EventLoop*> server2Ready;
        auto server2ReadyFuture = server2Ready.get_future();
        std::thread server2Thread([&server2Ready] {
            minireactor::EventLoop loop;
            minirpc::RpcServer server(
                &loop, minireactor::InetAddress("127.0.0.1", kPort), 1, 2, 16);
            registerEcho(server);
            server.start();
            server2Ready.set_value(&loop);
            loop.loop();
        });
        assert(server2ReadyFuture.wait_for(2s) == std::future_status::ready);
        minireactor::EventLoop* server2Loop = server2ReadyFuture.get();

        std::this_thread::sleep_for(1s);
        const minirpc::RpcResponse after =
            client.call({0, "TestService", "Echo", "world"}, 2s);
        assert(after.error_code == 0);
        assert(after.payload == "world");

        client.disconnect();
        server2Loop->quit();
        server2Thread.join();
    }
}
