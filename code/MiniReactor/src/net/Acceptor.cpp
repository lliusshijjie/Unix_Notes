#include "net/Acceptor.h"

#include "base/Logger.h"
#include "net/EventLoop.h"

#include <cerrno>
#include <string>
#include <utility>

namespace minireactor {

Acceptor::Acceptor(EventLoop* loop, std::uint16_t port, const char* bindIp)
    : loop_(loop),
      acceptSocket_(Socket::createNonblocking()),
      acceptChannel_(loop, acceptSocket_.fd()),
      port_(port),
      bindIp_(bindIp) {
    acceptSocket_.setReuseAddr(true);
    acceptChannel_.setReadCallback([this] { handleRead(); });
    acceptChannel_.enableEdgeTrigger();
}

void Acceptor::setNewConnectionCallback(NewConnectionCallback callback) {
    newConnectionCallback_ = std::move(callback);
}

void Acceptor::listen() {
    if (listening_) {
        return;
    }
    acceptSocket_.bindAddress(port_, bindIp_.c_str());
    acceptSocket_.listen();
    listening_ = true;
    acceptChannel_.enableReading();
    MR_LOG_INFO("acceptor listening on " + bindIp_ + ":" + std::to_string(port_));
}

void Acceptor::handleRead() {
    for (;;) {
        sockaddr_in peerAddress{};
        const int socketFd = acceptSocket_.accept(&peerAddress);
        if (socketFd >= 0) {
            if (newConnectionCallback_) {
                newConnectionCallback_(socketFd, peerAddress);
            }
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        break;  // EAGAIN/EWOULDBLOCK: every pending connection was accepted.
    }
}

}  // namespace minireactor
