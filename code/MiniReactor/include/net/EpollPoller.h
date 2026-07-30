#pragma once

#include "net/Poller.h"

#include <sys/epoll.h>

#include <unordered_map>
#include <vector>

namespace minireactor {

class EpollPoller : public Poller {
public:
    EpollPoller();
    ~EpollPoller() override;

    std::vector<Channel*> poll(int timeoutMs) override;
    void updateChannel(Channel* channel) override;
    void removeChannel(Channel* channel) override;

private:
    void update(int operation, Channel* channel);

    int epollFd_;
    std::vector<epoll_event> events_;
    std::unordered_map<int, Channel*> channels_;
};

}  // namespace minireactor
