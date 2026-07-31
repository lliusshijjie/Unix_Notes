#include "net/EventLoopThreadPool.h"

#include "net/EventLoop.h"
#include "net/EventLoopThread.h"

#include <stdexcept>

namespace minireactor {

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, std::size_t threadCount)
    : baseLoop_(baseLoop), threadCount_(threadCount) {}

EventLoopThreadPool::~EventLoopThreadPool() = default;

void EventLoopThreadPool::start() {
    if (started_) {
        throw std::logic_error("EventLoopThreadPool can only be started once");
    }
    baseLoop_->assertInLoopThread();
    started_ = true;
    threads_.reserve(threadCount_);
    loops_.reserve(threadCount_);
    for (std::size_t index = 0; index < threadCount_; ++index) {
        auto thread = std::make_unique<EventLoopThread>();
        loops_.push_back(thread->startLoop());
        threads_.push_back(std::move(thread));
    }
}

EventLoop* EventLoopThreadPool::getNextLoop() {
    if (!started_) {
        throw std::logic_error("EventLoopThreadPool is not started");
    }
    if (loops_.empty()) {
        return baseLoop_;
    }
    EventLoop* loop = loops_[next_];
    next_ = (next_ + 1) % loops_.size();
    return loop;
}

}  // namespace minireactor
