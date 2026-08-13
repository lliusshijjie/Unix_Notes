#include "base/Logger.h"
#include "net/EventLoop.h"
#include "net/TcpServer.h"

#include <cstdlib>
#include <exception>
#include <string>

int main(int argc, char* argv[]) {
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 8080);
    try {
        minireactor::EventLoop loop;
        minireactor::TcpServer server(&loop, port);
        server.setConnectionCallback([](const std::shared_ptr<minireactor::TcpConnection>& connection) {
            MR_LOG_INFO(connection->name() +
                        (connection->state() == minireactor::TcpConnection::State::kConnected
                             ? " connected"
                             : " disconnected"));
        });
        server.setMessageCallback([](const std::shared_ptr<minireactor::TcpConnection>& connection,
                                     minireactor::Buffer* buffer) {
            connection->send(buffer->retrieveAllAsString());
        });
        server.start();
        MR_LOG_INFO("echo server started on port " + std::to_string(port));
        loop.loop();
    } catch (const std::exception& error) {
        MR_LOG_ERROR(std::string("echo server failed: ") + error.what());
        return 1;
    }
}
