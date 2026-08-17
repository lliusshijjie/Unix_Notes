#include "mini_rpc/protocol.h"
#include "mini_rpc/rpc_call_options.h"
#include "mini_rpc/rpc_channel.h"
#include "mini_rpc/rpc_client.h"
#include "mini_rpc/rpc_server.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace {

constexpr std::uint16_t kAsyncPort = 39192;
constexpr std::uint16_t kReconnectPort = 39193;
constexpr std::uint16_t kRetryPort = 39195;

class TestServer {
public:
    explicit TestServer(std::uint16_t port)
        : port_(port) {
        thread_ = std::thread([this] { run(); });
        assert(ready_.get_future().wait_for(std::chrono::seconds(2)) ==
               std::future_status::ready);
    }

    ~TestServer() {
        stop();
    }

    void stop() {
        minireactor::EventLoop* loop = loop_.load(std::memory_order_acquire);
        if (loop != nullptr) {
            loop->quit();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void run() {
        minireactor::EventLoop loop;
        loop_.store(&loop, std::memory_order_release);
        minirpc::RpcServer server(
            &loop, minireactor::InetAddress("127.0.0.1", port_), 1, 4, 32);
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
        assert(server.registerMethod(
            "TestService", "Echo",
            [](const minirpc::RpcRequest& request, minirpc::RpcResponse& response) {
                response.payload = request.payload;
            }));
        server.start();
        ready_.set_value();
        loop.loop();
        loop_.store(nullptr, std::memory_order_release);
    }

    std::uint16_t port_;
    std::promise<void> ready_;
    std::atomic<minireactor::EventLoop*> loop_{nullptr};
    std::thread thread_;
};

minirpc::RpcRequest request(std::string method, std::string payload) {
    minirpc::RpcRequest result;
    result.service_name = "TestService";
    result.method_name = std::move(method);
    result.payload = std::move(payload);
    return result;
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    TestServer server(kAsyncPort);

    auto client = std::make_shared<minirpc::RpcClient>("127.0.0.1", kAsyncPort);
    std::mutex statesMutex;
    std::condition_variable statesCondition;
    bool sawConnected = false;
    bool sawDisconnected = false;
    client->setConnectionStateCallback(
        [&](minirpc::ClientConnectionState state) {
            {
                std::lock_guard<std::mutex> lock(statesMutex);
                sawConnected = sawConnected ||
                    state == minirpc::ClientConnectionState::Connected;
                sawDisconnected = sawDisconnected ||
                    state == minirpc::ClientConnectionState::Disconnected;
            }
            statesCondition.notify_all();
        });

    assert(client->connect(2s));
    {
        std::unique_lock<std::mutex> lock(statesMutex);
        assert(statesCondition.wait_for(
            lock, 1s, [&] { return sawConnected; }));
    }

    minirpc::RpcFuture future = client->callAsync(
        request("Echo", "future-value"));
    assert(future.waitFor(2s));
    const minirpc::RpcResponse futureResponse = future.get();
    assert(futureResponse.error_code == 0);
    assert(futureResponse.payload == "future-value");

    std::promise<minirpc::RpcResponse> callbackResult;
    client->callAsync(
        request("Echo", "callback-value"),
        [&callbackResult](minirpc::RpcResponse response) {
            callbackResult.set_value(std::move(response));
        });
    auto callbackFuture = callbackResult.get_future();
    assert(callbackFuture.wait_for(2s) == std::future_status::ready);
    const minirpc::RpcResponse callbackResponse = callbackFuture.get();
    assert(callbackResponse.error_code == 0);
    assert(callbackResponse.payload == "callback-value");

    minirpc::RpcCallOptions timeoutOptions;
    timeoutOptions.timeout = 30ms;
    minirpc::RpcFuture timedOut = client->callAsync(
        request("DelayEcho", "200 late"), timeoutOptions);
    assert(timedOut.waitFor(1s));
    assert(timedOut.get().error_code ==
           static_cast<int>(minirpc::RpcErrorCode::Timeout));
    std::this_thread::sleep_for(250ms);
    const minirpc::RpcResponse afterTimeout = client->call(
        request("Echo", "after-timeout"), 2s);
    assert(afterTimeout.error_code == 0);
    assert(afterTimeout.payload == "after-timeout");

    minirpc::RpcFuture cancelled = client->callAsync(
        request("DelayEcho", "200 cancelled"));
    cancelled.cancel();
    assert(cancelled.waitFor(1s));
    assert(cancelled.get().error_code ==
           static_cast<int>(minirpc::RpcErrorCode::Cancelled));
    std::this_thread::sleep_for(250ms);
    const minirpc::RpcResponse afterCancel = client->call(
        request("Echo", "after-cancel"), 2s);
    assert(afterCancel.error_code == 0);
    assert(afterCancel.payload == "after-cancel");

    minirpc::RpcChannel channel(client);
    minirpc::RpcFuture channelFuture = channel.callMethodAsync(
        "TestService", "Echo", "channel-future");
    assert(channelFuture.waitFor(2s));
    const minirpc::RpcResponse channelResponse = channelFuture.get();
    assert(channelResponse.error_code == 0);
    assert(channelResponse.payload == "channel-future");

    const minirpc::RpcMetricsSnapshot clientMetrics = client->metrics();
    assert(clientMetrics.client_total_calls == 7);
    assert(clientMetrics.client_success_calls == 5);
    assert(clientMetrics.client_timeout_calls == 1);
    assert(clientMetrics.client_cancelled_calls == 1);
    assert(clientMetrics.client_failed_calls == 2);
    assert(clientMetrics.client_pending_calls == 0);

    client->disconnect();
    {
        std::unique_lock<std::mutex> lock(statesMutex);
        assert(statesCondition.wait_for(
            lock, 1s, [&] { return sawDisconnected; }));
    }
    client.reset();
    server.stop();

    auto reconnectClient =
        std::make_shared<minirpc::RpcClient>("127.0.0.1", kReconnectPort);
    minirpc::RpcCallOptions queueOptions;
    queueOptions.fail_fast = false;
    queueOptions.timeout = 4s;

    minirpc::RpcFuture queued = reconnectClient->callAsync(
        request("Echo", "queued-until-server-starts"), queueOptions);
    assert(!queued.waitFor(200ms));

    TestServer reconnectServer(kReconnectPort);
    assert(queued.waitFor(3s));
    const minirpc::RpcResponse queuedResponse = queued.get();
    assert(queuedResponse.error_code == 0);
    assert(queuedResponse.payload == "queued-until-server-starts");
    assert(reconnectClient->connected());
    const minirpc::RpcMetricsSnapshot reconnectMetrics = reconnectClient->metrics();
    assert(reconnectMetrics.client_total_calls == 1);
    assert(reconnectMetrics.client_success_calls == 1);
    assert(reconnectMetrics.client_reconnect_count > 0);

    reconnectClient->disconnect();
    reconnectClient.reset();
    reconnectServer.stop();

    minirpc::RpcClient retryClient("127.0.0.1", kRetryPort);
    minirpc::RpcCallOptions retryOptions;
    retryOptions.fail_fast = false;
    retryOptions.retry_enabled = true;
    retryOptions.max_retries = 1;
    retryOptions.timeout = 3s;
    minirpc::RpcFuture retried = retryClient.callAsync(
        request("Echo", "never-sent"), retryOptions);
    assert(retried.waitFor(2s));
    assert(retried.get().error_code ==
           static_cast<int>(minirpc::RpcErrorCode::RetryExhausted));
    const minirpc::RpcMetricsSnapshot retryMetrics = retryClient.metrics();
    assert(retryMetrics.client_total_calls == 1);
    assert(retryMetrics.client_failed_calls == 1);
    retryClient.disconnect();
}
