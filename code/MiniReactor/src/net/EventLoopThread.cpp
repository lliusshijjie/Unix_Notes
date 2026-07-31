#include "net/EventLoopThread.h"

#include "net/EventLoop.h"

namespace minireactor {

EventLoopThread::EventLoopThread() = default;

EventLoopThread::~EventLoopThread() {
    EventLoop* loop = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop = loop_;
    }
    if (loop != nullptr) {
        loop->quit();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

EventLoop* EventLoopThread::startLoop() {
    thread_ = std::thread([this] { threadFunc(); });
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return loop_ != nullptr; });
    return loop_;
}

void EventLoopThread::threadFunc() {
    EventLoop loop;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = &loop;
        condition_.notify_one();
    }
    loop.loop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = nullptr;
    }
}

}  // namespace minireactor
