#pragma once

#include <google/protobuf/service.h>

#include <chrono>
#include <string>

namespace minirpc {

class RpcController : public google::protobuf::RpcController {
public:
    void reset();
    bool failed() const noexcept;
    int errorCode() const noexcept;
    const std::string& errorText() const noexcept;
    void setFailed(int errorCode, std::string errorMessage);
    void setTimeout(std::chrono::milliseconds timeout);
    std::chrono::milliseconds timeout() const noexcept;

    void Reset() override;
    bool Failed() const override;
    std::string ErrorText() const override;
    void StartCancel() override;
    void SetFailed(const std::string& reason) override;
    bool IsCanceled() const override;
    void NotifyOnCancel(google::protobuf::Closure* callback) override;

private:
    bool failed_{false};
    int errorCode_{0};
    std::string errorMessage_;
    std::chrono::milliseconds timeout_{3000};
};

}  // namespace minirpc
