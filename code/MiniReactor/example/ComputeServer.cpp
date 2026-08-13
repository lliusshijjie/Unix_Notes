// ComputeServer：IO 线程与业务线程解耦示例（BlockingQueue 轮子 + 背压）。
//
// 用法: ./compute_server [port] [businessThreads]
// 默认 port=8081, businessThreads=2。
//
// 演示点：
//   1. BlockingQueue<ComputeTask>：IO 线程投递任务（有界阻塞，天然背压），
//      业务线程取出处理；
//   2. 业务线程通过 TcpConnection::send 回发结果——send 内部 runInLoop 是线程安全的；
//   3. 连接断开后任务自然丢弃（weak_ptr.lock() 失败）。
#include "base/Logger.h"
#include "blocking_queue.hpp"
#include "net/EventLoop.h"
#include "net/TcpServer.h"

#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

// 业务任务：连接弱引用 + 原始负载
struct ComputeTask {
    std::weak_ptr<minireactor::TcpConnection> connection;
    std::string payload;
};

std::string process(const std::string& payload) {
    // 模拟耗时业务：翻转字符串
    return {payload.rbegin(), payload.rend()};
}

}  // namespace

int main(int argc, char* argv[]) {
    const auto port =
        static_cast<std::uint16_t>(argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 8081);
    const std::size_t businessThreads =
        static_cast<std::size_t>(argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 2);
    try {
        minireactor::EventLoop loop;
        minireactor::TcpServer server(&loop, port);

        BlockingQueue<ComputeTask> taskQueue(1024);

        // 业务线程：从队列取任务、处理、把结果发回连接
        std::vector<std::thread> workers;
        workers.reserve(businessThreads);
        for (std::size_t index = 0; index < businessThreads; ++index) {
            workers.emplace_back([&taskQueue] {
                for (;;) {
                    ComputeTask task;
                    if (!taskQueue.pop(task)) {
                        break;  // 队列已关闭
                    }
                    std::shared_ptr<minireactor::TcpConnection> connection =
                        task.connection.lock();
                    if (!connection) {
                        continue;  // 连接已断开，丢弃任务
                    }
                    connection->send(process(task.payload));
                }
            });
        }

        server.setConnectionCallback(
            [](const std::shared_ptr<minireactor::TcpConnection>& connection) {
                MR_LOG_INFO(connection->name() +
                            (connection->state() == minireactor::TcpConnection::State::kConnected
                                 ? " connected"
                                 : " disconnected"));
            });

        server.setMessageCallback(
            [&taskQueue](const std::shared_ptr<minireactor::TcpConnection>& connection,
                         minireactor::Buffer* buffer) {
                ComputeTask task;
                task.connection = connection;
                task.payload = buffer->retrieveAllAsString();
                // 有界阻塞队列：满时阻塞调用方（IO 线程），形成背压
                taskQueue.push(std::move(task));
            });

        server.start();
        MR_LOG_INFO("compute server listening on " + std::to_string(port) +
                    ", business threads " + std::to_string(businessThreads));
        loop.loop();

        // loop 退出后：关闭队列并等待业务线程退出
        taskQueue.close();
        for (std::thread& worker : workers) {
            worker.join();
        }
    } catch (const std::exception& error) {
        MR_LOG_ERROR(std::string("compute server failed: ") + error.what());
        return 1;
    }
}
