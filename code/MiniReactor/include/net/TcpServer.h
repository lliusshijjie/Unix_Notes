#pragma once

#include "base/NonCopyable.h"

#include "net/Acceptor.h"
#include "net/EventLoopThreadPool.h"
#include "net/TcpConnection.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace minireactor {

class EventLoop;

class TcpServer : private NonCopyable {
public:
    TcpServer(EventLoop* loop, std::uint16_t port, const char* bindIp = "0.0.0.0",
              std::size_t workerThreadCount = 0);
    void setConnectionCallback(TcpConnection::ConnectionCallback callback);
    void setMessageCallback(TcpConnection::MessageCallback callback);
    void setThreadNum(std::size_t threadCount);
    void start();

private:
    void newConnection(int socketFd, const sockaddr_in& peerAddress);
    void removeConnection(const std::shared_ptr<TcpConnection>& connection);
    void removeConnectionInLoop(const std::shared_ptr<TcpConnection>& connection);

    EventLoop* loop_;
    Acceptor acceptor_;
    std::unique_ptr<EventLoopThreadPool> threadPool_;
    std::size_t workerThreadCount_;
    TcpConnection::ConnectionCallback connectionCallback_;
    TcpConnection::MessageCallback messageCallback_;
    std::map<std::string, std::shared_ptr<TcpConnection>> connections_;
    int nextConnectionId_ = 1;
};

}  // namespace minireactor
