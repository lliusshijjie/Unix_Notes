#pragma once

#include "base/NonCopyable.h"

#include "net/Channel.h"
#include "net/Socket.h"

#include <functional>
#include <memory>
#include <netinet/in.h>
#include <string>

namespace minireactor {

class EventLoop;

class Acceptor : private NonCopyable {
public:
    using NewConnectionCallback = std::function<void(int socketFd, const sockaddr_in& peerAddress)>;

    Acceptor(EventLoop* loop, std::uint16_t port, const char* bindIp = "0.0.0.0");
    void setNewConnectionCallback(NewConnectionCallback callback);
    void listen();

private:
    void handleRead();

    EventLoop* loop_;
    Socket acceptSocket_;
    Channel acceptChannel_;
    const std::uint16_t port_;
    const std::string bindIp_;
    NewConnectionCallback newConnectionCallback_;
    bool listening_ = false;
};

}  // namespace minireactor
