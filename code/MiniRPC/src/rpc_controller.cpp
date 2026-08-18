#include "mini_rpc/rpc_controller.h"

#include "mini_rpc/protocol.h"

#include <stdexcept>
#include <utility>

namespace minirpc {

void RpcController::reset() {
    failed_ = false;
    errorCode_ = 0;
    errorMessage_.clear();
}

void RpcController::Reset() {
    reset();
}

bool RpcController::Failed() const {
    return failed();
}

std::string RpcController::ErrorText() const {
    return errorText();
}

void RpcController::StartCancel() {}

void RpcController::SetFailed(const std::string& reason) {
    setFailed(static_cast<int>(RpcErrorCode::ServerError), reason);
}

bool RpcController::IsCanceled() const {
    return false;
}

void RpcController::NotifyOnCancel(google::protobuf::Closure*) {}

bool RpcController::failed() const noexcept {
    return failed_;
}

int RpcController::errorCode() const noexcept {
    return errorCode_;
}

const std::string& RpcController::errorText() const noexcept {
    return errorMessage_;
}

void RpcController::setFailed(int errorCode, std::string errorMessage) {
    failed_ = true;
    errorCode_ = errorCode;
    errorMessage_ = std::move(errorMessage);
}

void RpcController::setTimeout(std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("RPC timeout must be greater than zero");
    }
    timeout_ = timeout;
}

std::chrono::milliseconds RpcController::timeout() const noexcept {
    return timeout_;
}

}  // namespace minirpc
