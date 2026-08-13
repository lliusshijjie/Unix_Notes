// IdleEchoServer：空闲连接超时断开示例（muduo idleconnection 风格）。
//
// 用法: ./idle_echo_server [port] [idleSeconds]
// 默认 port=8080, idleSeconds=5。
//
// 演示点：
//   1. EventLoop::runEvery 周期任务（每秒检查一次空闲连接）；
//   2. TcpConnection::forceCloseWithDelay 延迟主动断开；
//   3. IO 线程（worker loop）与主 loop 线程之间的跨线程数据共享（mutex 保护）。
#include "base/Logger.h"
#include "net/EventLoop.h"
#include "net/TcpServer.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace std::chrono_literals;

int main(int argc, char* argv[]) {
    const auto port =
        static_cast<std::uint16_t>(argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 8080);
    const double idleSeconds = argc > 2 ? std::strtod(argv[2], nullptr) : 5.0;
    try {
        minireactor::EventLoop loop;
        minireactor::TcpServer server(&loop, port);

        // messageCallback（worker loop 线程）与 runEvery 检查（主 loop 线程）
        // 并发访问以下两个 map，必须加锁。
        std::mutex mutex;
        std::map<std::string, std::weak_ptr<minireactor::TcpConnection>> connections;
        std::map<std::string, std::chrono::steady_clock::time_point> lastMessageTime;

        server.setConnectionCallback(
            [&](const std::shared_ptr<minireactor::TcpConnection>& connection) {
                if (connection->state() == minireactor::TcpConnection::State::kConnected) {
                    const auto now = std::chrono::steady_clock::now();
                    std::lock_guard<std::mutex> lock(mutex);
                    connections[connection->name()] = connection;
                    lastMessageTime[connection->name()] = now;
                    MR_LOG_INFO(connection->name() + " connected");
                } else {
                    std::lock_guard<std::mutex> lock(mutex);
                    connections.erase(connection->name());
                    lastMessageTime.erase(connection->name());
                    MR_LOG_INFO(connection->name() + " disconnected");
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

        // 每秒检查一次：超过 idleSeconds 没有消息的连接延迟 0.1s 后强制断开。
        // 回调经 EventLoop::runEvery 在 loop 线程执行。
        loop.runEvery(1.0, [&] {
            const auto now = std::chrono::steady_clock::now();
            std::vector<std::string> expired;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (const auto& [name, time] : lastMessageTime) {
                    if (now - time > std::chrono::duration<double>(idleSeconds)) {
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
                    MR_LOG_WARN(name + " idle for " + std::to_string(idleSeconds) +
                                "s, closing");
                    connection->forceCloseWithDelay(0.1);
                }
            }
        });

        MR_LOG_INFO("idle echo server listening on " + std::to_string(port) + ", idle timeout " +
                    std::to_string(idleSeconds) + "s");
        loop.loop();
    } catch (const std::exception& error) {
        MR_LOG_ERROR(std::string("idle echo server failed: ") + error.what());
        return 1;
    }
}
