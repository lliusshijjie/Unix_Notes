#pragma once

#include <chrono>
#include <string>

namespace minirpc {

class RpcController {
public:
    void reset();
    bool failed() const noexcept;
    int errorCode() const noexcept;
    const std::string& errorText() const noexcept;
    void setFailed(int errorCode, std::string errorMessage);
    void setTimeout(std::chrono::milliseconds timeout);
    std::chrono::milliseconds timeout() const noexcept;

private:
    bool failed_{false};
    int errorCode_{0};
    std::string errorMessage_;
    std::chrono::milliseconds timeout_{3000};
};

}  // namespace minirpc
