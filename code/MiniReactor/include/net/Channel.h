#pragma once

#include "base/NonCopyable.h"

#include <cstdint>
#include <functional>

namespace minireactor {

class EventLoop;

class Channel : private NonCopyable {
public:
    using EventCallback = std::function<void()>;
    enum class Index { kNew = -1, kAdded = 1, kDeleted = 2 };

    Channel(EventLoop* loop, int fd);

    void handleEvent();
    void setReadCallback(EventCallback callback);
    void setWriteCallback(EventCallback callback);
    void setCloseCallback(EventCallback callback);
    void setErrorCallback(EventCallback callback);

    int fd() const;
    std::uint32_t events() const;
    void setRevents(std::uint32_t revents);

    bool isNoneEvent() const;
    Index index() const;
    void setIndex(Index index);
    void enableReading();
    void disableReading();
    void enableWriting();
    void disableWriting();
    void disableAll();
    void enableEdgeTrigger();
    bool isWriting() const;
    void remove();

private:
    void update();

    EventLoop* loop_;
    const int fd_;
    std::uint32_t events_ = 0;
    std::uint32_t revents_ = 0;
    Index index_ = Index::kNew;
    EventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};

}  // namespace minireactor
