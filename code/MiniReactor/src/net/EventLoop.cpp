#include "net/EventLoop.h"

#include "net/Channel.h"
#include "net/EpollPoller.h"

#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <sys/eventfd.h>
#include <unistd.h>

namespace minireactor {

EventLoop::EventLoop()
    : poller_(std::make_unique<EpollPoller>())
    , threadId_(std::this_thread::get_id())
    , wakeupFd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (wakeupFd_ < 0) {
        throw std::runtime_error("eventfd failed");
    }
    wakeupChannel_ = std::make_unique<Channel>(this, wakeupFd_);
    wakeupChannel_->setReadCallback([this] { handleWakeupRead(); });
    wakeupChannel_->enableReading();
}

EventLoop::~EventLoop() {
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
