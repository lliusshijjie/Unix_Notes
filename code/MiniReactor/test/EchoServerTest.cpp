#include "net/EventLoop.h"
#include "net/TcpServer.h"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr std::uint16_t kPort = 39173;

int connectToServer() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(fd >= 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    assert(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    return fd;
}

void sendAll(int fd, const std::string& message) {
    std::size_t remaining = message.size();
    const char* data = message.data();
    while (remaining > 0) {
        const ssize_t written = ::send(fd, data, remaining, MSG_NOSIGNAL);
        assert(written > 0);
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }
}

std::string receiveAll(int fd, std::size_t expectedSize) {
    std::string received;
    received.resize(expectedSize);
    std::size_t offset = 0;
    while (offset < expectedSize) {
        const ssize_t count = ::read(fd, received.data() + offset, expectedSize - offset);
        assert(count > 0);
        offset += static_cast<std::size_t>(count);
    }
    return received;
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    std::promise<minireactor::EventLoop*> loopReady;
    std::promise<void> connectionsClosed;
    auto readyFuture = loopReady.get_future();
    auto closedFuture = connectionsClosed.get_future();
    std::atomic<int> closeCount{0};

    std::thread serverThread([&] {
        minireactor::EventLoop loop;
        minireactor::TcpServer server(&loop, kPort, "127.0.0.1", 2);
        server.setConnectionCallback([&](const std::shared_ptr<minireactor::TcpConnection>& connection) {
            if (connection->state() == minireactor::TcpConnection::State::kDisconnected &&
                closeCount.fetch_add(1) + 1 == 2) {
                connectionsClosed.set_value();
            }
        });
        server.setMessageCallback([](const std::shared_ptr<minireactor::TcpConnection>& connection,
                                     minireactor::Buffer* buffer) {
            connection->send(buffer->retrieveAllAsString());
        });
        server.start();
        loopReady.set_value(&loop);
        loop.loop();
    });

    assert(readyFuture.wait_for(2s) == std::future_status::ready);
    minireactor::EventLoop* serverLoop = readyFuture.get();

    const std::string firstPayload(32 * 1024, 'a');
    const std::string secondPayload(48 * 1024, 'b');
    const int firstClient = connectToServer();
    const int secondClient = connectToServer();
    sendAll(firstClient, firstPayload);
    sendAll(secondClient, secondPayload);
    assert(receiveAll(firstClient, firstPayload.size()) == firstPayload);
    assert(receiveAll(secondClient, secondPayload.size()) == secondPayload);
    ::close(firstClient);
    ::close(secondClient);

    assert(closedFuture.wait_for(2s) == std::future_status::ready);
    serverLoop->quit();
    serverThread.join();
}
