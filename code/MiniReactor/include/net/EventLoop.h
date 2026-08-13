#pragma once

#include "base/NonCopyable.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace minireactor {

class Channel;
class Poller;

class EventLoop : private NonCopyable {
public:
    using Functor = std::function<void()>;
    using TimerId = std::uint64_t;

    EventLoop();
    ~EventLoop();

    void loop();
    void quit();
    void runInLoop(Functor callback);
    void queueInLoop(Functor callback);

    // 定时器（muduo 风格）：回调在 loop 线程执行。
    // 底层由 TimerManager（TimerScheduler + ThreadPool 轮子）驱动。
    TimerId runAfter(double seconds, Functor callback);
    TimerId runEvery(double seconds, Functor callback);
    void cancel(TimerId id);

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

    // 定时器：alive 标志防止 EventLoop 析构后定时回调触碰悬垂 this；
    // timerIds_ 供析构时统一取消。
    std::shared_ptr<std::atomic<bool>> timerAlive_;
    std::mutex timerMutex_;
    std::unordered_set<TimerId> timerIds_;
};

}  // namespace minireactor
