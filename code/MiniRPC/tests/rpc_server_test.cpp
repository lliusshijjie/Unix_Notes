#include "mini_rpc/codec.h"
#include "mini_rpc/protocol.h"
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
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

constexpr std::uint16_t kPort = 39185;

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
            "CalculatorService", "Add",
            [](const minirpc::RpcRequest& request, minirpc::RpcResponse& response) {
                std::istringstream input(request.payload);
                int left = 0;
                int right = 0;
                input >> left >> right;
                if (!input) {
                    throw std::invalid_argument("invalid add payload");
                }
                response.payload = std::to_string(left + right);
            }));
        assert(server.registerMethod(
            "CalculatorService", "Throw",
            [](const minirpc::RpcRequest&, minirpc::RpcResponse&) {
                throw std::runtime_error("boom");
            }));
        assert(server.registerMethod(
            "CalculatorService", "OversizedResponse",
            [](const minirpc::RpcRequest&, minirpc::RpcResponse& response) {
                response.payload.assign(minirpc::kMaxMessageSize + 1, 'x');
            }));
        server.start();
        serverReady.set_value(&loop);
        loop.loop();
    });

    assert(serverReadyFuture.wait_for(2s) == std::future_status::ready);
    minireactor::EventLoop* serverLoop = serverReadyFuture.get();

    minireactor::EventLoopThread clientLoopThread;
    minireactor::EventLoop* clientLoop = clientLoopThread.startLoop();
    minirpc::RpcCodec codec;
    std::mutex responsesMutex;
    std::condition_variable responsesReady;
    std::unordered_map<std::uint64_t, minirpc::RpcResponse> responses;
    std::promise<void> connected;
    auto connectedFuture = connected.get_future();
    std::promise<void> disconnected;
    auto disconnectedFuture = disconnected.get_future();

    std::promise<std::shared_ptr<minireactor::TcpClient>> clientCreated;
    auto clientCreatedFuture = clientCreated.get_future();
    clientLoop->queueInLoop([&] {
        auto client = std::make_shared<minireactor::TcpClient>(
            clientLoop, minireactor::InetAddress("127.0.0.1", kPort), "rpc-test-client");
        client->setConnectionCallback(
            [&](const std::shared_ptr<minireactor::TcpConnection>& connection) {
                if (connection->state() == minireactor::TcpConnection::State::kConnected) {
                    connected.set_value();
                } else {
                    disconnected.set_value();
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
    frames += codec.encodeRequest({1001, "CalculatorService", "Add", "10 20"});
    frames += codec.encodeRequest({1002, "UnknownService", "Add", "10 20"});
    frames += codec.encodeRequest({1003, "CalculatorService", "Unknown", "10 20"});
    frames += codec.encodeRequest({1004, "CalculatorService", "Throw", ""});
    frames += codec.encodeRequest(
        {1007, "CalculatorService", "OversizedResponse", ""});
    connection->send(std::move(frames));

    {
        std::unique_lock<std::mutex> lock(responsesMutex);
        assert(responsesReady.wait_for(lock, 2s, [&responses] { return responses.size() == 5; }));
    }
    assert(responses.at(1001).error_code == 0);
    assert(responses.at(1001).payload == "30");
    assert(responses.at(1002).error_code ==
           static_cast<int>(minirpc::RpcErrorCode::ServiceNotFound));
    assert(responses.at(1003).error_code ==
           static_cast<int>(minirpc::RpcErrorCode::MethodNotFound));
    assert(responses.at(1004).error_code ==
           static_cast<int>(minirpc::RpcErrorCode::ServerError));
    assert(responses.at(1007).error_code ==
           static_cast<int>(minirpc::RpcErrorCode::ServerError));

    const std::string fragmented =
        codec.encodeRequest({1005, "CalculatorService", "Add", "7 8"});
    connection->send(fragmented.substr(0, 10));
    std::this_thread::sleep_for(20ms);
    connection->send(fragmented.substr(10));
    {
        std::unique_lock<std::mutex> lock(responsesMutex);
        assert(responsesReady.wait_for(lock, 2s, [&responses] { return responses.size() == 6; }));
    }
    assert(responses.at(1005).error_code == 0);
    assert(responses.at(1005).payload == "15");

    std::string invalid =
        codec.encodeRequest({1006, "CalculatorService", "Add", "1 2"});
    invalid[0] = 0;
    connection->send(std::move(invalid));
    assert(disconnectedFuture.wait_for(2s) == std::future_status::ready);

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
