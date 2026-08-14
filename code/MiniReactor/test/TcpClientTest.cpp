#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/EventLoopThread.h"
#include "net/InetAddress.h"
#include "net/TcpClient.h"
#include "net/TcpConnection.h"
#include "net/TcpServer.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr std::uint16_t kPort = 39184;

}  // namespace

int main() {
    using namespace std::chrono_literals;

    std::promise<minireactor::EventLoop*> serverReady;
    auto serverReadyFuture = serverReady.get_future();
    std::thread serverThread([&serverReady] {
        minireactor::EventLoop loop;
        minireactor::TcpServer server(&loop, kPort, "127.0.0.1", 1);
        server.setMessageCallback(
            [](const std::shared_ptr<minireactor::TcpConnection>& connection,
               minireactor::Buffer* buffer) {
                connection->send(buffer->retrieveAllAsString());
            });
        server.start();
        serverReady.set_value(&loop);
        loop.loop();
    });

    assert(serverReadyFuture.wait_for(2s) == std::future_status::ready);
    minireactor::EventLoop* serverLoop = serverReadyFuture.get();

    minireactor::EventLoopThread clientLoopThread;
    minireactor::EventLoop* clientLoop = clientLoopThread.startLoop();

    std::promise<void> connected;
    auto connectedFuture = connected.get_future();
    std::promise<void> reconnected;
    auto reconnectedFuture = reconnected.get_future();
    std::promise<void> disconnected;
    auto disconnectedFuture = disconnected.get_future();
    std::promise<std::string> echoed;
    auto echoedFuture = echoed.get_future();
    std::atomic<int> disconnectCount{0};
    std::atomic<int> connectCount{0};
    std::atomic<int> errorCount{0};
    std::mutex receivedMutex;
    std::string received;
    const std::string payload(64 * 1024, 'x');

    std::promise<std::shared_ptr<minireactor::TcpClient>> clientCreated;
    auto clientCreatedFuture = clientCreated.get_future();
    clientLoop->queueInLoop([&] {
        auto client = std::make_shared<minireactor::TcpClient>(
            clientLoop, minireactor::InetAddress("127.0.0.1", kPort), "echo-client");
        client->setConnectionCallback(
            [&](const std::shared_ptr<minireactor::TcpConnection>& connection) {
                if (connection->state() == minireactor::TcpConnection::State::kConnected) {
                    if (connectCount.fetch_add(1) == 0) {
                        connected.set_value();
                    } else {
                        reconnected.set_value();
                    }
                    return;
                }
                if (disconnectCount.fetch_add(1) == 0) {
                    disconnected.set_value();
                }
            });
        client->setMessageCallback(
            [&](const std::shared_ptr<minireactor::TcpConnection>&,
                minireactor::Buffer* buffer) {
                std::lock_guard<std::mutex> lock(receivedMutex);
                received += buffer->retrieveAllAsString();
                if (received.size() == payload.size()) {
                    echoed.set_value(received);
                }
            });
        client->setConnectionErrorCallback([&](int) { errorCount.fetch_add(1); });
        clientCreated.set_value(std::move(client));
    });

    assert(clientCreatedFuture.wait_for(2s) == std::future_status::ready);
    std::shared_ptr<minireactor::TcpClient> client = clientCreatedFuture.get();

    client->connect();
    assert(connectedFuture.wait_for(2s) == std::future_status::ready);
    assert(errorCount.load() == 0);

    std::shared_ptr<minireactor::TcpConnection> connection = client->connection();
    assert(connection != nullptr);
    connection->send(payload);
    assert(echoedFuture.wait_for(2s) == std::future_status::ready);
    assert(echoedFuture.get() == payload);

    client->disconnect();
    assert(disconnectedFuture.wait_for(2s) == std::future_status::ready);
    std::this_thread::sleep_for(50ms);
    assert(disconnectCount.load() == 1);

    connection.reset();
    client->connect();
    assert(reconnectedFuture.wait_for(2s) == std::future_status::ready);
    assert(connectCount.load() == 2);

    connection = client->connection();
    assert(connection != nullptr);
    client->stop();
    std::this_thread::sleep_for(50ms);
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
