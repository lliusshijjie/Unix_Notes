#pragma once

#include "base/NonCopyable.h"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace minireactor {

class EventLoop;

class EventLoopThread : private NonCopyable {
public:
    EventLoopThread();
    ~EventLoopThread();

    EventLoop* startLoop();

private:
    void threadFunc();

    std::mutex mutex_;
    std::condition_variable condition_;
    EventLoop* loop_ = nullptr;
    std::thread thread_;
};

}  // namespace minireactor
