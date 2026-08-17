#pragma once

#include <string>

namespace minirpc {

class RpcSerializer {
public:
    virtual ~RpcSerializer() = default;

    virtual std::string name() const = 0;
    virtual std::string serialize(const std::string& payload) const = 0;
    virtual bool deserialize(const std::string& bytes, std::string& payload,
                             std::string& errorMessage) const = 0;
};

class RawStringSerializer final : public RpcSerializer {
public:
    std::string name() const override { return "raw"; }

    std::string serialize(const std::string& payload) const override {
        return payload;
    }

    bool deserialize(const std::string& bytes, std::string& payload,
                     std::string& errorMessage) const override {
        payload = bytes;
        errorMessage.clear();
        return true;
    }
};

}  // namespace minirpc
