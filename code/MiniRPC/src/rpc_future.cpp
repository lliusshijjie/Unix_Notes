#include "mini_rpc/rpc_future.h"

#include <stdexcept>
#include <utility>

namespace minirpc {

RpcFuture::RpcFuture()
    : state_(std::make_shared<State>()) {}

RpcFuture::RpcFuture(std::shared_ptr<State> state)
    : state_(std::move(state)) {
    if (!state_) {
        throw std::invalid_argument("RpcFuture requires state");
    }
}

RpcResponse RpcFuture::get() {
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->condition.wait(lock, [this] { return state_->completed; });
    return state_->response;
}

bool RpcFuture::wait() {
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->condition.wait(lock, [this] { return state_->completed; });
    return true;
}

bool RpcFuture::waitFor(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(state_->mutex);
    return state_->condition.wait_for(
        lock, timeout, [this] { return state_->completed; });
}

bool RpcFuture::ready() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->completed;
}

void RpcFuture::cancel() {
    std::function<void()> cancel;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->completed) {
            return;
        }
        cancel = state_->cancel;
    }
    if (cancel) {
        cancel();
    }
}

void RpcFuture::complete(const std::shared_ptr<State>& state, RpcResponse response) {
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->completed) {
            return;
        }
        state->response = std::move(response);
        state->completed = true;
        state->cancel = {};
    }
    state->condition.notify_all();
}

void RpcFuture::setCancel(const std::shared_ptr<State>& state,
                          std::function<void()> cancel) {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->completed) {
        state->cancel = std::move(cancel);
    }
}

}  // namespace minirpc
