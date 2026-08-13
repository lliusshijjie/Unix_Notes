#pragma once

#include "base/NonCopyable.h"

#include "net/Buffer.h"
#include "net/Channel.h"
#include "net/Socket.h"

#include <functional>
#include <memory>
#include <string>

namespace minireactor {

class EventLoop;

class TcpConnection : public std::enable_shared_from_this<TcpConnection>, private NonCopyable {
public:
    enum class State { kConnecting, kConnected, kDisconnecting, kDisconnected };
    using ConnectionCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using MessageCallback = std::function<void(const std::shared_ptr<TcpConnection>&, Buffer*)>;
    using CloseCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;

    TcpConnection(EventLoop* loop, std::string name, int socketFd);
    ~TcpConnection() = default;

    const std::string& name() const;
    State state() const;
    EventLoop* loop() const;
    void setConnectionCallback(ConnectionCallback callback);
    void setMessageCallback(MessageCallback callback);
    void setCloseCallback(CloseCallback callback);

    void connectEstablished();
    void connectDestroyed();
    void send(std::string message);
    void shutdown();
    // 主动断开连接（muduo 风格）。forceCloseWithDelay 在 delay 秒后断开，
    // 由 EventLoop 定时器驱动，可用于空闲连接超时。
    void forceClose();
    void forceCloseWithDelay(double seconds);

private:
    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();
    void sendInLoop(std::string message);
    void shutdownInLoop();
    void forceCloseInLoop();

    EventLoop* loop_;
    const std::string name_;
    State state_ = State::kDisconnected;
    Socket socket_;
    Channel channel_;
    Buffer inputBuffer_;
    Buffer outputBuffer_;
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    CloseCallback closeCallback_;
};

}  // namespace minireactor
