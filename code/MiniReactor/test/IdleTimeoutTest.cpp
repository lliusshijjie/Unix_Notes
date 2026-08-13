// 端到端测试：空闲连接被服务端超时主动断开。
// 覆盖：EventLoop::runEvery 周期检查 + TcpConnection::forceCloseWithDelay +
//       多 worker loop 下跨线程共享状态（mutex 保护）。
#include "base/Logger.h"
#include "net/EventLoop.h"
#include "net/TcpServer.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
constexpr std::uint16_t kPort = 39175;
constexpr double kIdleSeconds = 0.5;
constexpr double kCheckInterval = 0.1;
}  // namespace

int main() {
    using namespace std::chrono_literals;

    std::promise<minireactor::EventLoop*> loopReady;
    auto readyFuture = loopReady.get_future();

    std::mutex mutex;
    std::map<std::string, std::weak_ptr<minireactor::TcpConnection>> connections;
    std::map<std::string, std::chrono::steady_clock::time_point> lastMessageTime;

    std::thread serverThread([&] {
        minireactor::EventLoop loop;
        minireactor::TcpServer server(&loop, kPort, "127.0.0.1", 2);
        server.setConnectionCallback(
            [&](const std::shared_ptr<minireactor::TcpConnection>& connection) {
                if (connection->state() == minireactor::TcpConnection::State::kConnected) {
                    std::lock_guard<std::mutex> lock(mutex);
                    connections[connection->name()] = connection;
                    lastMessageTime[connection->name()] = std::chrono::steady_clock::now();
                } else {
                    std::lock_guard<std::mutex> lock(mutex);
                    connections.erase(connection->name());
                    lastMessageTime.erase(connection->name());
                }
            });
        server.setMessageCallback(
            [&](const std::shared_ptr<minireactor::TcpConnection>& connection,
                minireactor::Buffer* buffer) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    lastMessageTime[connection->name()] = std::chrono::steady_clock::now();
                }
                connection->send(buffer->retrieveAllAsString());
            });
        server.start();
        loop.runEvery(kCheckInterval, [&] {
            const auto now = std::chrono::steady_clock::now();
            std::vector<std::string> expired;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (const auto& [name, time] : lastMessageTime) {
                    if (now - time > std::chrono::duration<double>(kIdleSeconds)) {
                        expired.push_back(name);
                    }
                }
            }
            for (const std::string& name : expired) {
                std::shared_ptr<minireactor::TcpConnection> connection;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    const auto found = connections.find(name);
                    if (found != connections.end()) {
                        connection = found->second.lock();
                        connections.erase(found);
                        lastMessageTime.erase(name);
                    }
                }
                if (connection) {
                    connection->forceCloseWithDelay(0.1);
                }
            }
        });
        loopReady.set_value(&loop);
        loop.loop();
    });

    assert(readyFuture.wait_for(2s) == std::future_status::ready);
    minireactor::EventLoop* serverLoop = readyFuture.get();

    // 客户端：连接 → 发送 → 收到回显 → 保持静默，等待服务端超时断开
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    assert(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);

    const std::string payload = "hello";
    assert(::send(fd, payload.data(), payload.size(), MSG_NOSIGNAL) ==
           static_cast<ssize_t>(payload.size()));

    char buffer[64];
    std::size_t received = 0;
    while (received < payload.size()) {
        const ssize_t count = ::read(fd, buffer + received, sizeof(buffer) - received);
        assert(count > 0);
        received += static_cast<std::size_t>(count);
    }
    assert(std::string(buffer, received) == payload);

    // 静默等待：服务端应在 idle + 检查周期内主动关闭连接
    pollfd pollDescriptor{fd, POLLIN, 0};
    const int pollResult = ::poll(&pollDescriptor, 1, 3000);
    assert(pollResult == 1);
    char byte = 0;
    assert(::read(fd, &byte, 1) == 0);  // 服务端已关闭

    ::close(fd);
    serverLoop->quit();
    serverThread.join();
    return 0;
}
