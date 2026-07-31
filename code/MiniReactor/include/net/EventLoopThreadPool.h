#pragma once

#include "base/NonCopyable.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace minireactor {

class EventLoop;
class EventLoopThread;

class EventLoopThreadPool : private NonCopyable {
public:
    EventLoopThreadPool(EventLoop* baseLoop, std::size_t threadCount);
    ~EventLoopThreadPool();

    void start();
    EventLoop* getNextLoop();

private:
    EventLoop* baseLoop_;
    const std::size_t threadCount_;
    bool started_ = false;
    std::size_t next_ = 0;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};

}  // namespace minireactor
