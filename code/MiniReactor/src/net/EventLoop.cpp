#include "net/EventLoop.h"

#include "base/TimerManager.h"
#include "net/Channel.h"
#include "net/EpollPoller.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <sys/eventfd.h>
#include <unistd.h>

namespace minireactor {

namespace {

std::chrono::steady_clock::duration secondsToDuration(double seconds) {
    return std::chrono::nanoseconds(static_cast<std::int64_t>(seconds * 1e9));
}

}  // namespace

EventLoop::EventLoop()
    : poller_(std::make_unique<EpollPoller>())
    , threadId_(std::this_thread::get_id())
    , wakeupFd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC))
    , timerAlive_(std::make_shared<std::atomic<bool>>(true)) {
    if (wakeupFd_ < 0) {
        throw std::runtime_error("eventfd failed");
    }
    wakeupChannel_ = std::make_unique<Channel>(this, wakeupFd_);
    wakeupChannel_->setReadCallback([this] { handleWakeupRead(); });
    wakeupChannel_->enableReading();
}

EventLoop::~EventLoop() {
    // 先让在途定时回调失效（它们可能正持有指向本对象的裸指针），再取消全部定时器。
    timerAlive_->store(false, std::memory_order_release);
    std::unordered_set<TimerId> ids;
    {
        std::lock_guard<std::mutex> lock(timerMutex_);
        ids.swap(timerIds_);
    }
    for (const TimerId id : ids) {
        TimerManager::instance().cancel(id);
    }
    wakeupChannel_->disableAll();
    wakeupChannel_->remove();
    ::close(wakeupFd_);
}

void EventLoop::loop() {
    assertInLoopThread();
    while (!quit_.load(std::memory_order_acquire)) {
        for (Channel* channel : poller_->poll(-1)) {
            channel->handleEvent();
        }
        doPendingFunctors();
    }
}

void EventLoop::quit() {
    quit_.store(true, std::memory_order_release);
    if (!isInLoopThread()) {
        wakeup();
    }
}

void EventLoop::runInLoop(Functor callback) {
    if (isInLoopThread()) {
        callback();
        return;
    }
    queueInLoop(std::move(callback));
}

void EventLoop::queueInLoop(Functor callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingFunctors_.push_back(std::move(callback));
    }
    if (!isInLoopThread() || callingPendingFunctors_) {
        wakeup();
    }
}

bool EventLoop::isInLoopThread() const noexcept {
    return threadId_ == std::this_thread::get_id();
}

EventLoop::TimerId EventLoop::runAfter(double seconds, Functor callback) {
    std::shared_ptr<std::atomic<bool>> alive = timerAlive_;
    const TimerId id = TimerManager::instance().runAfter(
        secondsToDuration(seconds),
        [this, alive, callback]() mutable {
            if (alive->load(std::memory_order_acquire)) {
                // 拷贝而不是 move：周期定时器会多次调用同一个包装 lambda
                queueInLoop(callback);
            }
        });
    {
        std::lock_guard<std::mutex> lock(timerMutex_);
        timerIds_.insert(id);
    }
    return id;
}

EventLoop::TimerId EventLoop::runEvery(double seconds, Functor callback) {
    std::shared_ptr<std::atomic<bool>> alive = timerAlive_;
    const TimerId id = TimerManager::instance().runEvery(
        secondsToDuration(seconds),
        [this, alive, callback]() mutable {
            if (alive->load(std::memory_order_acquire)) {
                // 拷贝而不是 move：周期定时器会多次调用同一个包装 lambda
                queueInLoop(callback);
            }
        });
    {
        std::lock_guard<std::mutex> lock(timerMutex_);
        timerIds_.insert(id);
    }
    return id;
}

void EventLoop::cancel(TimerId id) {
    {
        std::lock_guard<std::mutex> lock(timerMutex_);
        timerIds_.erase(id);
    }
    TimerManager::instance().cancel(id);
}

void EventLoop::assertInLoopThread() const {
    if (!isInLoopThread()) {
        throw std::logic_error("EventLoop used from a non-owner thread");
    }
}

void EventLoop::updateChannel(Channel* channel) {
    assertInLoopThread();
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel) {
    assertInLoopThread();
    poller_->removeChannel(channel);
}

void EventLoop::wakeup() {
    const std::uint64_t one = 1;
    const ssize_t written = ::write(wakeupFd_, &one, sizeof(one));
    if (written != static_cast<ssize_t>(sizeof(one)) && errno != EAGAIN) {
        throw std::runtime_error("EventLoop wakeup write failed");
    }
}

void EventLoop::handleWakeupRead() {
    std::uint64_t value = 0;
    while (::read(wakeupFd_, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value))) {
    }
}

void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);
    }
    for (const Functor& functor : functors) {
        functor();
    }
    callingPendingFunctors_ = false;
}

}  // namespace minireactor
