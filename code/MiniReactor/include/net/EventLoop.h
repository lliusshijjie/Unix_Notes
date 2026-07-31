#pragma once

#include "base/NonCopyable.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace minireactor {

class Channel;
class Poller;

class EventLoop : private NonCopyable {
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    void loop();
    void quit();
    void runInLoop(Functor callback);
    void queueInLoop(Functor callback);

    bool isInLoopThread() const noexcept;
    void assertInLoopThread() const;
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);

private:
    void wakeup();
    void handleWakeupRead();
    void doPendingFunctors();

    std::atomic<bool> quit_{false};
    std::unique_ptr<Poller> poller_;
    const std::thread::id threadId_;
    int wakeupFd_;
    std::unique_ptr<Channel> wakeupChannel_;
    std::mutex mutex_;
    std::vector<Functor> pendingFunctors_;
    bool callingPendingFunctors_ = false;
};

}  // namespace minireactor
