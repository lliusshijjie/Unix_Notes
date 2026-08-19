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

// 实例 A：Echo 慢（3s，用于触发超时）、Slow 慢（3s）
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

// 实例 B：Echo 立即返回（带实例标识）、Slow 慢（3s）
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

// 验证自动重试：可重试错误（Timeout）换实例重试、预算约束、关闭重试。
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
        // 用例 1：Timeout 重试换实例成功。
        // 预算 2s 均分给 2 次尝试 → 第一次打 A（慢）1s 超时，重试打 B（快）成功。
        minirpc::RpcClient client("TestService", discovery);
        client.setMaxRetries(1);
        assert(client.connect(2s));

        const auto started = std::chrono::steady_clock::now();
        const minirpc::RpcResponse response =
            client.call({0, "TestService", "Echo", "value"}, 2s);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        assert(response.error_code == 0);
        assert(response.payload == "value@B");   // 重试确实换到了 B
        assert(elapsed >= 700ms);                // 第一次确实验证了超时
        assert(elapsed < 1900ms);

        client.disconnect();
    }

    {
        // 用例 2：预算约束——A、B 都慢时，总耗时 ≈ timeout，不翻倍。
        minirpc::RpcClient client("TestService", discovery);
        client.setMaxRetries(1);
        assert(client.connect(2s));

        const auto started = std::chrono::steady_clock::now();
        const minirpc::RpcResponse response =
            client.call({0, "TestService", "Slow", ""}, 1s);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        assert(response.error_code == static_cast<int>(minirpc::RpcErrorCode::Timeout));
        assert(elapsed >= 800ms);
        assert(elapsed < 1500ms);   // 预算控制下总耗时约 1s；失控会到 2s+

        client.disconnect();
    }

    {
        // 用例 3：setMaxRetries(0) 关闭重试，行为与 Phase2 一致（单次尝试）。
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
