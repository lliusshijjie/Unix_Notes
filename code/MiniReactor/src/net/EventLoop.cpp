#include "net/EventLoop.h"

#include "net/Channel.h"
#include "net/EpollPoller.h"

namespace minireactor {

EventLoop::EventLoop() : poller_(std::make_unique<EpollPoller>()) {}
EventLoop::~EventLoop() = default;

void EventLoop::loop() {
    quit_ = false;
    while (!quit_) {
        for (Channel* channel : poller_->poll(-1)) {
            channel->handleEvent();
        }
    }
}

void EventLoop::quit() { quit_ = true; }
void EventLoop::updateChannel(Channel* channel) { poller_->updateChannel(channel); }
void EventLoop::removeChannel(Channel* channel) { poller_->removeChannel(channel); }

}  // namespace minireactor
