#pragma once

#include "base/NonCopyable.h"
#include "net/InetAddress.h"

#include <atomic>
#include <functional>
#include <memory>

namespace minireactor {

class Channel;
class EventLoop;

class Connector : public std::enable_shared_from_this<Connector>, private NonCopyable {
public:
    using NewConnectionCallback = std::function<void(int)>;
    using ErrorCallback = std::function<void(int)>;

    Connector(EventLoop* loop, InetAddress serverAddress);
    ~Connector();

    void setNewConnectionCallback(NewConnectionCallback callback);
    void setErrorCallback(ErrorCallback callback);

    void start();
    void stop();

private:
    enum class State { kDisconnected, kConnecting, kConnected };

    void startInLoop();
    void stopInLoop();
    void connecting(int socketFd);
    void handleWrite();
    void handleError();
    int removeAndResetChannel();
    int socketError(int socketFd) const;
    void reportError(int socketFd, int error);

    EventLoop* loop_;
    InetAddress serverAddress_;
    std::atomic<bool> connectRequested_{false};
    State state_{State::kDisconnected};
    int socketFd_{-1};
    std::unique_ptr<Channel> channel_;
    NewConnectionCallback newConnectionCallback_;
    ErrorCallback errorCallback_;
};

}  // namespace minireactor
