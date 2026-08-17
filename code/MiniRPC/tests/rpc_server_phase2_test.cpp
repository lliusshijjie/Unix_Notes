#include "mini_rpc/codec.h"
#include "mini_rpc/protocol.h"
#include "mini_rpc/rpc_metrics.h"
#include "mini_rpc/rpc_server.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/EventLoopThread.h"
#include "net/InetAddress.h"
#include "net/TcpClient.h"
#include "net/TcpConnection.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

constexpr std::uint16_t kPort = 39194;

minirpc::RpcRequest makeRequest(std::uint64_t id, std::string service,
                                std::string method, std::string payload,
                                std::string traceId,
                                std::uint64_t timeoutMs = 1000) {
    minirpc::RpcRequest request;
    request.request_id = id;
    request.service_name = std::move(service);
    request.method_name = std::move(method);
    request.payload = std::move(payload);
    request.trace_id = std::move(traceId);
    request.serializer = "raw";
    request.timeout_ms = timeoutMs;
    request.attempt = 1;
    return request;
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    std::promise<minireactor::EventLoop*> serverReady;
    std::promise<minirpc::RpcServer*> serverObject;
    auto serverReadyFuture = serverReady.get_future();
    auto serverObjectFuture = serverObject.get_future();
    std::thread serverThread([&serverReady, &serverObject] {
        minireactor::EventLoop loop;
        minirpc::RpcServer server(
            &loop, minireactor::InetAddress("127.0.0.1", kPort), 1, 2, 16);
        assert(server.registerMethod(
            "ObservedService", "Echo",
            [](const minirpc::RpcRequest& request, minirpc::RpcResponse& response) {
                assert(request.trace_id == "trace-ok");
                assert(request.serializer == "raw");
                assert(request.timeout_ms == 1000);
                response.payload = request.payload;
            }));
        assert(server.registerMethod(
            "ObservedService", "Slow",
            [](const minirpc::RpcRequest&, minirpc::RpcResponse& response) {
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                response.payload = "late";
            }));
        assert(server.registerMethod(
            "ObservedService", "Throw",
            [](const minirpc::RpcRequest&, minirpc::RpcResponse&) {
                throw std::runtime_error("boom");
            }));
        server.start();
        serverObject.set_value(&server);
        serverReady.set_value(&loop);
        loop.loop();
    });

    assert(serverReadyFuture.wait_for(2s) == std::future_status::ready);
    minireactor::EventLoop* serverLoop = serverReadyFuture.get();
    assert(serverObjectFuture.wait_for(2s) == std::future_status::ready);
    minirpc::RpcServer* server = serverObjectFuture.get();

    minireactor::EventLoopThread clientLoopThread;
    minireactor::EventLoop* clientLoop = clientLoopThread.startLoop();
    minirpc::RpcCodec codec;
    std::mutex responsesMutex;
    std::condition_variable responsesReady;
    std::unordered_map<std::uint64_t, minirpc::RpcResponse> responses;
    std::promise<void> connected;
    auto connectedFuture = connected.get_future();

    std::promise<std::shared_ptr<minireactor::TcpClient>> clientCreated;
    auto clientCreatedFuture = clientCreated.get_future();
    clientLoop->queueInLoop([&] {
        auto client = std::make_shared<minireactor::TcpClient>(
            clientLoop, minireactor::InetAddress("127.0.0.1", kPort),
            "rpc-server-phase2-client");
        client->setConnectionCallback(
            [&](const std::shared_ptr<minireactor::TcpConnection>& connection) {
                if (connection->state() == minireactor::TcpConnection::State::kConnected) {
                    connected.set_value();
                }
            });
        client->setMessageCallback(
            [&](const std::shared_ptr<minireactor::TcpConnection>&,
                minireactor::Buffer* buffer) {
                for (;;) {
                    minirpc::RpcResponse response;
                    const minirpc::DecodeStatus status =
                        codec.tryDecodeResponse(*buffer, response);
                    if (status == minirpc::DecodeStatus::NeedMoreData) {
                        return;
                    }
                    assert(status == minirpc::DecodeStatus::Complete);
                    {
                        std::lock_guard<std::mutex> lock(responsesMutex);
                        responses.emplace(response.request_id, std::move(response));
                    }
                    responsesReady.notify_one();
                }
            });
        clientCreated.set_value(std::move(client));
    });

    assert(clientCreatedFuture.wait_for(2s) == std::future_status::ready);
    std::shared_ptr<minireactor::TcpClient> client = clientCreatedFuture.get();
    client->connect();
    assert(connectedFuture.wait_for(2s) == std::future_status::ready);
    std::shared_ptr<minireactor::TcpConnection> connection = client->connection();
    assert(connection != nullptr);

    std::string frames;
    frames += codec.encodeRequest(
        makeRequest(2001, "ObservedService", "Echo", "ok", "trace-ok"));
    frames += codec.encodeRequest(
        makeRequest(2002, "ObservedService", "Slow", "", "trace-slow", 20));
    frames += codec.encodeRequest(
        makeRequest(2003, "ObservedService", "Throw", "", "trace-throw"));
    frames += codec.encodeRequest(
        makeRequest(2004, "UnknownService", "Echo", "", "trace-missing"));
    minirpc::RpcRequest badSerializer =
        makeRequest(2005, "ObservedService", "Echo", "bad", "trace-bad-serializer");
    badSerializer.serializer = "unknown";
    frames += codec.encodeRequest(badSerializer);
    connection->send(std::move(frames));

    {
        std::unique_lock<std::mutex> lock(responsesMutex);
        assert(responsesReady.wait_for(lock, 3s, [&responses] {
            return responses.size() == 5;
        }));
    }

    assert(responses.at(2001).error_code == 0);
    assert(responses.at(2001).payload == "ok");
    assert(responses.at(2001).trace_id == "trace-ok");
    assert(responses.at(2001).serializer == "raw");
    assert(responses.at(2001).server_cost_us > 0);

    assert(responses.at(2002).error_code ==
           static_cast<int>(minirpc::RpcErrorCode::Timeout));
    assert(responses.at(2002).trace_id == "trace-slow");
    assert(responses.at(2002).server_cost_us > 0);

    assert(responses.at(2003).error_code ==
           static_cast<int>(minirpc::RpcErrorCode::ServerError));
    assert(responses.at(2003).trace_id == "trace-throw");

    assert(responses.at(2004).error_code ==
           static_cast<int>(minirpc::RpcErrorCode::ServiceNotFound));
    assert(responses.at(2004).trace_id == "trace-missing");

    assert(responses.at(2005).error_code ==
           static_cast<int>(minirpc::RpcErrorCode::DeserializationError));
    assert(responses.at(2005).trace_id == "trace-bad-serializer");

    const minirpc::RpcMetricsSnapshot metrics = server->metrics();
    assert(metrics.server_total_requests == 5);
    assert(metrics.server_success_responses == 1);
    assert(metrics.server_timeout_responses == 1);
    assert(metrics.server_service_not_found == 1);
    assert(metrics.server_error_responses == 1);
    assert(metrics.server_current_queue_size == 0);

    connection.reset();
    std::promise<void> clientDestroyed;
    auto clientDestroyedFuture = clientDestroyed.get_future();
    clientLoop->queueInLoop([&client, &clientDestroyed] {
        client->stop();
        client.reset();
        clientDestroyed.set_value();
    });
    assert(clientDestroyedFuture.wait_for(2s) == std::future_status::ready);
    clientLoop->quit();
    serverLoop->quit();
    serverThread.join();
}
