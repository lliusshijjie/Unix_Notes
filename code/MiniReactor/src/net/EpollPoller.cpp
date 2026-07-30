#include "net/EpollPoller.h"

#include "net/Channel.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace minireactor {

namespace {
constexpr int kInitialEventListSize = 16;

[[noreturn]] void throwSystemError(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}
}  // namespace

EpollPoller::EpollPoller()
    : epollFd_(::epoll_create1(EPOLL_CLOEXEC)), events_(kInitialEventListSize) {
    if (epollFd_ < 0) {
        throwSystemError("epoll_create1");
    }
}

EpollPoller::~EpollPoller() {
    ::close(epollFd_);
}

std::vector<Channel*> EpollPoller::poll(int timeoutMs) {
    const int eventCount = ::epoll_wait(epollFd_, events_.data(),
                                        static_cast<int>(events_.size()), timeoutMs);
    if (eventCount < 0) {
        if (errno == EINTR) {
            return {};
        }
        throwSystemError("epoll_wait");
    }

    std::vector<Channel*> activeChannels;
    activeChannels.reserve(static_cast<std::size_t>(eventCount));
    for (int index = 0; index < eventCount; ++index) {
        auto* channel = static_cast<Channel*>(events_[index].data.ptr);
        channel->setRevents(events_[index].events);
        activeChannels.push_back(channel);
    }
    if (eventCount == static_cast<int>(events_.size())) {
        events_.resize(events_.size() * 2);
    }
    return activeChannels;
}

void EpollPoller::updateChannel(Channel* channel) {
    const auto found = channels_.find(channel->fd());
    if (found == channels_.end()) {
        channels_.emplace(channel->fd(), channel);
        update(EPOLL_CTL_ADD, channel);
    } else {
        update(EPOLL_CTL_MOD, channel);
    }
}

void EpollPoller::removeChannel(Channel* channel) {
    const auto found = channels_.find(channel->fd());
    if (found == channels_.end()) {
        return;
    }
    if (::epoll_ctl(epollFd_, EPOLL_CTL_DEL, channel->fd(), nullptr) < 0 && errno != ENOENT) {
        throwSystemError("epoll_ctl DEL");
    }
    channels_.erase(found);
}

void EpollPoller::update(int operation, Channel* channel) {
    epoll_event event{};
    event.events = channel->events();
    event.data.ptr = channel;
    if (::epoll_ctl(epollFd_, operation, channel->fd(), &event) < 0) {
        throwSystemError(operation == EPOLL_CTL_ADD ? "epoll_ctl ADD" : "epoll_ctl MOD");
    }
}

}  // namespace minireactor
