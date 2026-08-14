#pragma once

#include "mini_rpc/protocol.h"

#include <string>

namespace minireactor {
class Buffer;
}

namespace minirpc {

enum class DecodeStatus {
    Complete,
    NeedMoreData,
    ProtocolError
};

class RpcCodec {
public:
    std::string encodeRequest(const RpcRequest& request) const;
    std::string encodeResponse(const RpcResponse& response) const;

    DecodeStatus tryDecodeRequest(minireactor::Buffer& buffer, RpcRequest& request) const;
    DecodeStatus tryDecodeResponse(minireactor::Buffer& buffer, RpcResponse& response) const;
};

}  // namespace minirpc
