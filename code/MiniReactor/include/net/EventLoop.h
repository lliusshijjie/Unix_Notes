#pragma once

#include "base/NonCopyable.h"

#include <memory>

namespace minireactor {

class Channel;
class Poller;

class EventLoop : private NonCopyable {
public:
    EventLoop();
    ~EventLoop();

    void loop();
    void quit();
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);

private:
    bool quit_ = false;
    std::unique_ptr<Poller> poller_;
};

}  // namespace minireactor
