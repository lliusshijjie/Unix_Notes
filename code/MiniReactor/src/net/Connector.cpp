#include "net/Connector.h"

#include "net/Channel.h"
#include "net/EventLoop.h"
#include "net/Socket.h"

#include <cassert>
#include <cerrno>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace minireactor {

Connector::Connector(EventLoop* loop, InetAddress serverAddress)
    : loop_(loop), serverAddress_(std::move(serverAddress)) {
    if (loop_ == nullptr) {
        throw std::invalid_argument("Connector requires an EventLoop");
    }
}

Connector::~Connector() {
    assert(channel_ == nullptr);
    if (socketFd_ >= 0) {
        ::close(socketFd_);
    }
}

void Connector::setNewConnectionCallback(NewConnectionCallback callback) {
    newConnectionCallback_ = std::move(callback);
}

void Connector::setErrorCallback(ErrorCallback callback) {
    errorCallback_ = std::move(callback);
}

void Connector::start() {
    connectRequested_.store(true, std::memory_order_release);
    std::shared_ptr<Connector> self = shared_from_this();
    loop_->runInLoop([self] { self->startInLoop(); });
}

void Connector::stop() {
    connectRequested_.store(false, std::memory_order_release);
    std::shared_ptr<Connector> self = shared_from_this();
    loop_->runInLoop([self] { self->stopInLoop(); });
}

void Connector::startInLoop() {
    loop_->assertInLoopThread();
    if (!connectRequested_.load(std::memory_order_acquire) || state_ != State::kDisconnected) {
        return;
    }

    int socketFd = -1;
    try {
        socketFd = Socket::createNonblocking();
    } catch (const std::system_error& error) {
        if (errorCallback_) {
            errorCallback_(error.code().value());
        }
        return;
    }

    const sockaddr_in& address = serverAddress_.sockaddr();
    const int result = ::connect(socketFd, reinterpret_cast<const sockaddr*>(&address),
                                 sizeof(address));
    const int error = result == 0 ? 0 : errno;
    switch (error) {
    case 0:
        state_ = State::kConnected;
        if (connectRequested_.load(std::memory_order_acquire) && newConnectionCallback_) {
            newConnectionCallback_(socketFd);
        } else {
            ::close(socketFd);
            state_ = State::kDisconnected;
        }
        break;
    case EINPROGRESS:
    case EINTR:
    case EISCONN:
        connecting(socketFd);
        break;
    default:
        reportError(socketFd, error);
        break;
    }
}

void Connector::stopInLoop() {
    loop_->assertInLoopThread();
    if (state_ == State::kConnecting) {
        const int socketFd = removeAndResetChannel();
        state_ = State::kDisconnected;
        ::close(socketFd);
        return;
    }
    if (state_ == State::kConnected) {
        state_ = State::kDisconnected;
    }
}

void Connector::connecting(int socketFd) {
    loop_->assertInLoopThread();
    state_ = State::kConnecting;
    socketFd_ = socketFd;
    channel_ = std::make_unique<Channel>(loop_, socketFd_);

    std::weak_ptr<Connector> weakSelf = shared_from_this();
    channel_->setWriteCallback([weakSelf] {
        if (std::shared_ptr<Connector> self = weakSelf.lock()) {
            self->handleWrite();
        }
    });
    channel_->setErrorCallback([weakSelf] {
        if (std::shared_ptr<Connector> self = weakSelf.lock()) {
            self->handleError();
        }
    });
    channel_->setCloseCallback([weakSelf] {
        if (std::shared_ptr<Connector> self = weakSelf.lock()) {
            self->handleError();
        }
    });
    channel_->enableWriting();
}

void Connector::handleWrite() {
    loop_->assertInLoopThread();
    if (state_ != State::kConnecting) {
        return;
    }

    const int socketFd = removeAndResetChannel();
    const int error = socketError(socketFd);
    if (error != 0) {
        state_ = State::kDisconnected;
        reportError(socketFd, error);
        return;
    }
    if (!connectRequested_.load(std::memory_order_acquire)) {
        state_ = State::kDisconnected;
        ::close(socketFd);
        return;
    }

    state_ = State::kConnected;
    if (newConnectionCallback_) {
        newConnectionCallback_(socketFd);
    } else {
        ::close(socketFd);
        state_ = State::kDisconnected;
    }
}

void Connector::handleError() {
    loop_->assertInLoopThread();
    if (state_ != State::kConnecting) {
        return;
    }

    const int error = socketError(socketFd_);
    const int socketFd = removeAndResetChannel();
    state_ = State::kDisconnected;
    reportError(socketFd, error == 0 ? ECONNABORTED : error);
}

int Connector::removeAndResetChannel() {
    loop_->assertInLoopThread();
    assert(channel_ != nullptr);

    channel_->disableAll();
    channel_->remove();
    const int socketFd = socketFd_;
    socketFd_ = -1;

    std::shared_ptr<Connector> self = shared_from_this();
    loop_->queueInLoop([self] { self->channel_.reset(); });
    return socketFd;
}

int Connector::socketError(int socketFd) const {
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(socketFd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) {
        return errno;
    }
    return error;
}

void Connector::reportError(int socketFd, int error) {
    ::close(socketFd);
    if (connectRequested_.load(std::memory_order_acquire) && errorCallback_) {
        errorCallback_(error);
    }
}

}  // namespace minireactor
