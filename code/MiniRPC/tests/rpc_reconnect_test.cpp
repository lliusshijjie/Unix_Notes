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

// 验证自动重连：server 关闭后 client 自动退避重连，server 恢复后调用自动可用。
int main() {
    using namespace std::chrono_literals;

    // Phase A：第一个 server 启动，client 连接成功
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
        client.setReconnectPolicy(100ms, 500ms);   // 测试用短退避
        assert(client.connect(2s));

        const minirpc::RpcResponse before =
            client.call({0, "TestService", "Echo", "hello"}, 2s);
        assert(before.error_code == 0);
        assert(before.payload == "hello");

        // Phase B：关闭 server1，等待 client 感知断连并开始退避重连
        server1Loop->quit();
        server1Thread.join();
        std::this_thread::sleep_for(600ms);

        // Phase C：同一端口启动 server2，等待 client 自动重连成功
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

        // 等 client 自动重连成功（退避 100ms/200ms/400ms，1s 足够）
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
