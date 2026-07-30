#pragma once

#include "base/NonCopyable.h"

#include <vector>

namespace minireactor {

class Channel;

class Poller : private NonCopyable {
public:
    virtual ~Poller() = default;

    virtual std::vector<Channel*> poll(int timeoutMs) = 0;
    virtual void updateChannel(Channel* channel) = 0;
    virtual void removeChannel(Channel* channel) = 0;
};

}  // namespace minireactor
