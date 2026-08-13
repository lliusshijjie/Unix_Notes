#include "net/TcpServer.h"

#include "base/Logger.h"
#include "net/EventLoop.h"

#include <stdexcept>
#include <utility>

namespace minireactor {

TcpServer::TcpServer(EventLoop* loop, std::uint16_t port, const char* bindIp,
                     std::size_t workerThreadCount)
    : loop_(loop),
      acceptor_(loop, port, bindIp),
      workerThreadCount_(workerThreadCount) {
    acceptor_.setNewConnectionCallback(
        [this](int socketFd, const sockaddr_in& peerAddress) { newConnection(socketFd, peerAddress); });
}

void TcpServer::setConnectionCallback(TcpConnection::ConnectionCallback callback) {
    connectionCallback_ = std::move(callback);
}

void TcpServer::setMessageCallback(TcpConnection::MessageCallback callback) {
    messageCallback_ = std::move(callback);
}

void TcpServer::setThreadNum(std::size_t threadCount) {
    if (threadPool_ != nullptr) {
        throw std::logic_error("TcpServer has already started");
    }
    workerThreadCount_ = threadCount;
}

void TcpServer::start() {
    loop_->assertInLoopThread();
    if (threadPool_ == nullptr) {
        threadPool_ = std::make_unique<EventLoopThreadPool>(loop_, workerThreadCount_);
        threadPool_->start();
    }
    acceptor_.listen();
}

void TcpServer::newConnection(int socketFd, const sockaddr_in&) {
    loop_->assertInLoopThread();
    EventLoop* ioLoop = threadPool_->getNextLoop();
    const std::string connectionName = "connection-" + std::to_string(nextConnectionId_++);
    auto connection = std::make_shared<TcpConnection>(ioLoop, connectionName, socketFd);
    connection->setConnectionCallback(connectionCallback_);
    connection->setMessageCallback(messageCallback_);
    connection->setCloseCallback([this](const std::shared_ptr<TcpConnection>& item) {
        removeConnection(item);
    });
    connections_.emplace(connectionName, connection);
    ioLoop->runInLoop([connection] { connection->connectEstablished(); });
    MR_LOG_DEBUG(connectionName + " accepted, assigned to worker loop");
}

void TcpServer::removeConnection(const std::shared_ptr<TcpConnection>& connection) {
    loop_->queueInLoop([this, connection] { removeConnectionInLoop(connection); });
}

void TcpServer::removeConnectionInLoop(const std::shared_ptr<TcpConnection>& connection) {
    loop_->assertInLoopThread();
    const auto found = connections_.find(connection->name());
    if (found == connections_.end()) {
        return;
    }
    connections_.erase(found);
    MR_LOG_DEBUG(connection->name() + " removed from server");
    connection->loop()->queueInLoop([connection] { connection->connectDestroyed(); });
}

}  // namespace minireactor
