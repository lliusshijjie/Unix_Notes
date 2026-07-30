#include "net/Socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <sys/socket.h>
#include <unistd.h>

namespace minireactor {

namespace {
[[noreturn]] void throwSystemError(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}
}  // namespace

Socket::Socket(int fd) : fd_(fd) {}
Socket::~Socket() { ::close(fd_); }

int Socket::createNonblocking() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) {
        throwSystemError("socket");
    }
    return fd;
}

int Socket::fd() const { return fd_; }

void Socket::setReuseAddr(bool enabled) {
    const int value = enabled ? 1 : 0;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)) < 0) {
        throwSystemError("setsockopt SO_REUSEADDR");
    }
}

void Socket::bindAddress(std::uint16_t port, const char* ipAddress) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, ipAddress, &address.sin_addr) != 1) {
        throw std::invalid_argument("bind address must be a valid IPv4 address");
    }
    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        throwSystemError("bind");
    }
}

void Socket::listen() {
    if (::listen(fd_, SOMAXCONN) < 0) {
        throwSystemError("listen");
    }
}

int Socket::accept(sockaddr_in* peerAddress) const {
    socklen_t length = sizeof(*peerAddress);
    const int connectionFd = ::accept4(fd_, reinterpret_cast<sockaddr*>(peerAddress), &length,
                                       SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (connectionFd < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        throwSystemError("accept4");
    }
    return connectionFd;
}

void Socket::shutdownWrite() const {
    if (::shutdown(fd_, SHUT_WR) < 0 && errno != ENOTCONN) {
        throwSystemError("shutdown write");
    }
}

}  // namespace minireactor
