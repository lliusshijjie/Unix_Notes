#include "net/TcpConnection.h"

#include "base/Logger.h"
#include "net/EventLoop.h"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace minireactor {

TcpConnection::TcpConnection(EventLoop* loop, std::string name, int socketFd)
    : loop_(loop),
      name_(std::move(name)),
      state_(State::kConnecting),
      socket_(socketFd),
      channel_(loop, socketFd) {
    channel_.setReadCallback([this] { handleRead(); });
    channel_.setWriteCallback([this] { handleWrite(); });
    channel_.setCloseCallback([this] { handleClose(); });
    channel_.setErrorCallback([this] { handleError(); });
    channel_.enableEdgeTrigger();
}

const std::string& TcpConnection::name() const { return name_; }
TcpConnection::State TcpConnection::state() const { return state_; }
EventLoop* TcpConnection::loop() const { return loop_; }

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
    loop_->assertInLoopThread();
    state_ = State::kConnected;
    channel_.enableReading();
    MR_LOG_DEBUG(name_ + " established on loop thread");
    if (connectionCallback_) {
        connectionCallback_(shared_from_this());
    }
}

void TcpConnection::connectDestroyed() {
    loop_->assertInLoopThread();
    if (state_ != State::kDisconnected) {
        state_ = State::kDisconnected;
        channel_.disableAll();
    }
    channel_.remove();
    MR_LOG_DEBUG(name_ + " destroyed");
    if (connectionCallback_) {
        connectionCallback_(shared_from_this());
    }
}

void TcpConnection::send(std::string message) {
    auto self = shared_from_this();
    loop_->runInLoop([self, message = std::move(message)]() mutable {
        self->sendInLoop(std::move(message));
    });
}

void TcpConnection::sendInLoop(std::string message) {
    loop_->assertInLoopThread();
    if (state_ != State::kConnected || message.empty()) {
        return;
    }

    std::size_t remaining = message.size();
    const char* data = message.data();
    if (!channel_.isWriting() && outputBuffer_.readableBytes() == 0) {
        for (;;) {
            const ssize_t written = ::send(socket_.fd(), data, remaining, MSG_NOSIGNAL);
            if (written > 0) {
                remaining -= static_cast<std::size_t>(written);
                data += written;
                if (remaining == 0) {
                    break;
                }
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
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
    auto self = shared_from_this();
    loop_->runInLoop([self] { self->shutdownInLoop(); });
}

void TcpConnection::shutdownInLoop() {
    loop_->assertInLoopThread();
    if (state_ == State::kConnected) {
        state_ = State::kDisconnecting;
        if (!channel_.isWriting()) {
            socket_.shutdownWrite();
        }
    }
}

void TcpConnection::forceClose() {
    auto self = shared_from_this();
    loop_->runInLoop([self] { self->forceCloseInLoop(); });
}

void TcpConnection::forceCloseInLoop() {
    loop_->assertInLoopThread();
    if (state_ == State::kConnected || state_ == State::kDisconnecting) {
        MR_LOG_DEBUG(name_ + " force closed");
        handleClose();
    }
}

void TcpConnection::forceCloseWithDelay(double seconds) {
    auto self = shared_from_this();
    loop_->runAfter(seconds, [self] { self->forceClose(); });
}

void TcpConnection::handleRead() {
    loop_->assertInLoopThread();
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
            if (receivedData && messageCallback_) {
                messageCallback_(shared_from_this(), &inputBuffer_);
            }
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
    loop_->assertInLoopThread();
    if (!channel_.isWriting()) {
        return;
    }

    while (outputBuffer_.readableBytes() > 0) {
        const ssize_t written = ::send(socket_.fd(), outputBuffer_.peek(),
                                       outputBuffer_.readableBytes(), MSG_NOSIGNAL);
        if (written > 0) {
            outputBuffer_.retrieve(static_cast<std::size_t>(written));
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        handleClose();
        return;
    }

    channel_.disableWriting();
    if (state_ == State::kDisconnecting) {
        socket_.shutdownWrite();
    }
}

void TcpConnection::handleClose() {
    loop_->assertInLoopThread();
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
    if (::getsockopt(socket_.fd(), SOL_SOCKET, SO_ERROR, &socketError, &length) == 0 &&
        socketError != 0) {
        MR_LOG_ERROR(name_ + " socket error: " + std::strerror(socketError));
    }
    handleClose();
}

}  // namespace minireactor
