#include "net/TcpConnection.h"

#include "net/EventLoop.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace minireactor {

TcpConnection::TcpConnection(EventLoop* loop, std::string name, int socketFd)
    : loop_(loop), name_(std::move(name)), socket_(socketFd), channel_(loop, socketFd) {
    channel_.setReadCallback([this] { handleRead(); });
    channel_.setWriteCallback([this] { handleWrite(); });
    channel_.setCloseCallback([this] { handleClose(); });
    channel_.setErrorCallback([this] { handleError(); });
}

const std::string& TcpConnection::name() const { return name_; }
TcpConnection::State TcpConnection::state() const { return state_; }

void TcpConnection::setConnectionCallback(ConnectionCallback callback) {
    connectionCallback_ = std::move(callback);
}

void TcpConnection::setMessageCallback(MessageCallback callback) {
    messageCallback_ = std::move(callback);
}

void TcpConnection::setCloseCallback(CloseCallback callback) {
    closeCallback_ = std::move(callback);
}

void TcpConnection::connectEstablished() {
    state_ = State::kConnected;
    channel_.enableReading();
    if (connectionCallback_) {
        connectionCallback_(shared_from_this());
    }
}

void TcpConnection::connectDestroyed() {
    if (state_ == State::kConnected) {
        state_ = State::kDisconnected;
        channel_.disableAll();
    }
    channel_.remove();
    if (connectionCallback_) {
        connectionCallback_(shared_from_this());
    }
}

void TcpConnection::send(const std::string& message) {
    if (state_ != State::kConnected || message.empty()) {
        return;
    }

    std::size_t remaining = message.size();
    const char* data = message.data();
    if (!channel_.isWriting() && outputBuffer_.readableBytes() == 0) {
        const ssize_t written = ::send(socket_.fd(), data, remaining, MSG_NOSIGNAL);
        if (written > 0) {
            remaining -= static_cast<std::size_t>(written);
            data += written;
        } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            handleClose();
            return;
        }
    }
    if (remaining > 0) {
        outputBuffer_.append(data, remaining);
        if (!channel_.isWriting()) {
            channel_.enableWriting();
        }
    }
}

void TcpConnection::shutdown() {
    if (state_ == State::kConnected && !channel_.isWriting()) {
        socket_.shutdownWrite();
    }
}

void TcpConnection::handleRead() {
    char buffer[4096];
    bool receivedData = false;
    for (;;) {
        const ssize_t bytesRead = ::read(socket_.fd(), buffer, sizeof(buffer));
        if (bytesRead > 0) {
            inputBuffer_.append(buffer, static_cast<std::size_t>(bytesRead));
            receivedData = true;
            continue;
        }
        if (bytesRead == 0) {
            handleClose();
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        handleClose();
        return;
    }
    if (receivedData && messageCallback_) {
        messageCallback_(shared_from_this(), &inputBuffer_);
    }
}

void TcpConnection::handleWrite() {
    if (!channel_.isWriting()) {
        return;
    }
    const ssize_t written = ::send(socket_.fd(), outputBuffer_.peek(),
                                   outputBuffer_.readableBytes(), MSG_NOSIGNAL);
    if (written > 0) {
        outputBuffer_.retrieve(static_cast<std::size_t>(written));
        if (outputBuffer_.readableBytes() == 0) {
            channel_.disableWriting();
        }
    } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        handleClose();
    }
}

void TcpConnection::handleClose() {
    if (state_ == State::kDisconnected) {
        return;
    }
    state_ = State::kDisconnected;
    channel_.disableAll();
    if (closeCallback_) {
        closeCallback_(shared_from_this());
    }
}

void TcpConnection::handleError() {
    int socketError = 0;
    socklen_t length = sizeof(socketError);
    if (::getsockopt(socket_.fd(), SOL_SOCKET, SO_ERROR, &socketError, &length) == 0 && socketError != 0) {
        std::cerr << name_ << " socket error: " << std::strerror(socketError) << '\n';
    }
    handleClose();
}

}  // namespace minireactor
