#pragma once

#include "base/NonCopyable.h"
#include "net/InetAddress.h"
#include "net/TcpConnection.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace minireactor {

class Connector;
class EventLoop;

class TcpClient : private NonCopyable {
public:
    using ConnectionErrorCallback = std::function<void(int)>;

    TcpClient(EventLoop* loop, const InetAddress& serverAddress, std::string name);
    ~TcpClient();

    void setConnectionCallback(TcpConnection::ConnectionCallback callback);
    void setMessageCallback(TcpConnection::MessageCallback callback);
    void setConnectionErrorCallback(ConnectionErrorCallback callback);

    void connect();
    void disconnect();
    void stop();
    std::shared_ptr<TcpConnection> connection() const;

private:
    void newConnection(int socketFd);
    void removeConnection(const std::shared_ptr<TcpConnection>& connection);

    EventLoop* loop_;
    const InetAddress serverAddress_;
    const std::string name_;
    std::shared_ptr<Connector> connector_;
    mutable std::mutex mutex_;
    std::shared_ptr<TcpConnection> connection_;
    TcpConnection::ConnectionCallback connectionCallback_;
    TcpConnection::MessageCallback messageCallback_;
    ConnectionErrorCallback connectionErrorCallback_;
    int nextConnectionId_{1};
};

}  // namespace minireactor
