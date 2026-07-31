#include "net/Channel.h"
#include "net/EventLoop.h"

#include <sys/epoll.h>

namespace minireactor {

Channel::Channel(EventLoop* loop, int fd) : loop_(loop), fd_(fd) {}

void Channel::handleEvent() {
    if ((revents_ & EPOLLHUP) != 0 && (revents_ & EPOLLIN) == 0) {
        if (closeCallback_) {
            closeCallback_();
        }
        return;
    }
    if ((revents_ & EPOLLERR) != 0 && errorCallback_) {
        errorCallback_();
        return;
    }
    if ((revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) != 0 && readCallback_) {
        readCallback_();
    }
    if ((revents_ & EPOLLOUT) != 0 && writeCallback_) {
        writeCallback_();
    }
}

void Channel::setReadCallback(EventCallback callback) { readCallback_ = std::move(callback); }
void Channel::setWriteCallback(EventCallback callback) { writeCallback_ = std::move(callback); }
void Channel::setCloseCallback(EventCallback callback) { closeCallback_ = std::move(callback); }
void Channel::setErrorCallback(EventCallback callback) { errorCallback_ = std::move(callback); }

int Channel::fd() const { return fd_; }
std::uint32_t Channel::events() const { return events_; }
void Channel::setRevents(std::uint32_t revents) { revents_ = revents; }
bool Channel::isNoneEvent() const { return events_ == 0; }
Channel::Index Channel::index() const { return index_; }
void Channel::setIndex(Index index) { index_ = index; }

void Channel::enableReading() { events_ |= EPOLLIN | EPOLLRDHUP; update(); }
void Channel::disableReading() { events_ &= ~(EPOLLIN | EPOLLRDHUP); update(); }
void Channel::enableWriting() { events_ |= EPOLLOUT; update(); }
void Channel::disableWriting() { events_ &= ~EPOLLOUT; update(); }
void Channel::disableAll() { events_ = 0; update(); }
void Channel::enableEdgeTrigger() { events_ |= EPOLLET; }
bool Channel::isWriting() const { return (events_ & EPOLLOUT) != 0; }
void Channel::remove() { loop_->removeChannel(this); }
void Channel::update() { loop_->updateChannel(this); }

}  // namespace minireactor
