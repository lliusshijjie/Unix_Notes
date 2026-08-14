#include "net/Connector.h"
#include "net/EventLoop.h"
#include "net/EventLoopThread.h"
#include "net/InetAddress.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

int createListener(std::uint16_t& port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(fd >= 0);

    const int enabled = 1;
    assert(::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    assert(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    assert(::listen(fd, 8) == 0);

    socklen_t length = sizeof(address);
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    port = ntohs(address.sin_port);
    return fd;
}

void testSuccessfulConnection() {
    using namespace std::chrono_literals;

    std::uint16_t port = 0;
    const int listener = createListener(port);
    std::thread acceptThread([listener] {
        const int accepted = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        assert(accepted >= 0);
        ::close(accepted);
    });

    minireactor::EventLoopThread loopThread;
    minireactor::EventLoop* loop = loopThread.startLoop();
    auto connector = std::make_shared<minireactor::Connector>(
        loop, minireactor::InetAddress("127.0.0.1", port));

    std::promise<int> connected;
    auto connectedFuture = connected.get_future();
    connector->setNewConnectionCallback([&connected](int socketFd) {
        connected.set_value(socketFd);
    });
    connector->start();

    assert(connectedFuture.wait_for(2s) == std::future_status::ready);
    const int socketFd = connectedFuture.get();
    assert(socketFd >= 0);
    ::close(socketFd);

    connector->stop();
    loop->quit();
    acceptThread.join();
    ::close(listener);
}

void testRefusedConnection() {
    using namespace std::chrono_literals;

    std::uint16_t port = 0;
    const int listener = createListener(port);
    ::close(listener);

    minireactor::EventLoopThread loopThread;
    minireactor::EventLoop* loop = loopThread.startLoop();
    auto connector = std::make_shared<minireactor::Connector>(
        loop, minireactor::InetAddress("127.0.0.1", port));

    std::promise<int> refused;
    auto refusedFuture = refused.get_future();
    connector->setErrorCallback([&refused](int error) {
        refused.set_value(error);
    });
    connector->start();

    assert(refusedFuture.wait_for(2s) == std::future_status::ready);
    assert(refusedFuture.get() != 0);
    connector->stop();
    loop->quit();
}

void testStopCancelsPendingDelivery() {
    using namespace std::chrono_literals;

    std::uint16_t port = 0;
    const int listener = createListener(port);

    minireactor::EventLoopThread loopThread;
    minireactor::EventLoop* loop = loopThread.startLoop();
    auto connector = std::make_shared<minireactor::Connector>(
        loop, minireactor::InetAddress("127.0.0.1", port));

    std::promise<int> delivered;
    auto deliveredFuture = delivered.get_future();
    connector->setNewConnectionCallback([&delivered](int socketFd) {
        delivered.set_value(socketFd);
        ::close(socketFd);
    });

    std::promise<void> stopped;
    auto stoppedFuture = stopped.get_future();
    loop->queueInLoop([connector, loop, &stopped] {
        connector->start();
        connector->stop();
        loop->queueInLoop([&stopped] { stopped.set_value(); });
    });

    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
    assert(deliveredFuture.wait_for(200ms) == std::future_status::timeout);
    loop->quit();
    ::close(listener);
}

}  // namespace

int main() {
    testSuccessfulConnection();
    testRefusedConnection();
    testStopCancelsPendingDelivery();
}
