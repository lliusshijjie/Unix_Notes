#include "net/EventLoop.h"
#include "net/TcpServer.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 8080);
    try {
        minireactor::EventLoop loop;
        minireactor::TcpServer server(&loop, port);
        server.setConnectionCallback([](const std::shared_ptr<minireactor::TcpConnection>& connection) {
            std::cout << connection->name()
                      << (connection->state() == minireactor::TcpConnection::State::kConnected
                              ? " connected\n"
                              : " disconnected\n");
        });
        server.setMessageCallback([](const std::shared_ptr<minireactor::TcpConnection>& connection,
                                     minireactor::Buffer* buffer) {
            connection->send(buffer->retrieveAllAsString());
        });
        server.start();
        std::cout << "server listening " << port << '\n';
        loop.loop();
    } catch (const std::exception& error) {
        std::cerr << "echo server failed: " << error.what() << '\n';
        return 1;
    }
}
