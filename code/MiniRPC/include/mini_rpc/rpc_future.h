#pragma once

#include "mini_rpc/protocol.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

namespace minirpc {

class RpcClient;

class RpcFuture {
public:
    RpcFuture();

    RpcResponse get();
    bool wait();
    bool waitFor(std::chrono::milliseconds timeout);
    bool ready() const;
    void cancel();

private:
    struct State {
        mutable std::mutex mutex;
        std::condition_variable condition;
        bool completed{false};
        RpcResponse response;
        std::function<void()> cancel;
    };

    explicit RpcFuture(std::shared_ptr<State> state);

    static void complete(const std::shared_ptr<State>& state, RpcResponse response);
    static void setCancel(const std::shared_ptr<State>& state, std::function<void()> cancel);

    std::shared_ptr<State> state_;

    friend class RpcClient;
};

}  // namespace minirpc
