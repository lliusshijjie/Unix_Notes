#include "mini_rpc/codec.h"
#include "mini_rpc/protocol.h"
#include "net/Buffer.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

void expectRequestProtocolError(const minirpc::RpcCodec& codec, const std::string& frame) {
    minireactor::Buffer buffer;
    buffer.append(frame);
    const std::size_t originalSize = buffer.readableBytes();
    minirpc::RpcRequest request;
    assert(codec.tryDecodeRequest(buffer, request) == minirpc::DecodeStatus::ProtocolError);
    assert(buffer.readableBytes() == originalSize);
}

void testRequestRoundTripAndFragmentation() {
    minirpc::RpcCodec codec;
    const minirpc::RpcRequest request{
        42, "CalculatorService", "Add", std::string("a\0b", 3)};
    const std::string frame = codec.encodeRequest(request);

    minireactor::Buffer fragmented;
    for (std::size_t index = 0; index + 1 < frame.size(); ++index) {
        fragmented.append(frame.data() + index, 1);
        minirpc::RpcRequest decoded;
        assert(codec.tryDecodeRequest(fragmented, decoded) ==
               minirpc::DecodeStatus::NeedMoreData);
    }
    fragmented.append(frame.data() + frame.size() - 1, 1);

    minirpc::RpcRequest decoded;
    assert(codec.tryDecodeRequest(fragmented, decoded) == minirpc::DecodeStatus::Complete);
    assert(decoded.request_id == request.request_id);
    assert(decoded.service_name == request.service_name);
    assert(decoded.method_name == request.method_name);
    assert(decoded.payload == request.payload);
    assert(fragmented.readableBytes() == 0);
}

void testResponseRoundTrip() {
    minirpc::RpcCodec codec;
    const minirpc::RpcResponse response{77, 2002, "method missing", "result"};
    minireactor::Buffer buffer;
    buffer.append(codec.encodeResponse(response));

    minirpc::RpcResponse decoded;
    assert(codec.tryDecodeResponse(buffer, decoded) == minirpc::DecodeStatus::Complete);
    assert(decoded.request_id == response.request_id);
    assert(decoded.error_code == response.error_code);
    assert(decoded.error_message == response.error_message);
    assert(decoded.payload == response.payload);
}

void testStickyPackets() {
    minirpc::RpcCodec codec;
    minireactor::Buffer buffer;
    for (std::uint64_t id = 1; id <= 3; ++id) {
        buffer.append(codec.encodeRequest({id, "Service", "Method", std::to_string(id)}));
    }

    for (std::uint64_t id = 1; id <= 3; ++id) {
        minirpc::RpcRequest decoded;
        assert(codec.tryDecodeRequest(buffer, decoded) == minirpc::DecodeStatus::Complete);
        assert(decoded.request_id == id);
        assert(decoded.payload == std::to_string(id));
    }
    assert(buffer.readableBytes() == 0);
}

void testInvalidFramesAreRejectedWithoutConsumption() {
    minirpc::RpcCodec codec;
    const std::string valid = codec.encodeRequest({9, "Service", "Method", "body"});

    std::string badMagic = valid;
    badMagic[0] = 0;
    expectRequestProtocolError(codec, badMagic);

    std::string badVersion = valid;
    badVersion[4] = 0;
    badVersion[5] = 2;
    expectRequestProtocolError(codec, badVersion);

    std::string badType = valid;
    badType[6] = 0;
    badType[7] = 2;
    expectRequestProtocolError(codec, badType);

    std::string oversized = valid;
    oversized[20] = static_cast<char>(0xff);
    oversized[21] = static_cast<char>(0xff);
    oversized[22] = static_cast<char>(0xff);
    oversized[23] = static_cast<char>(0xff);
    expectRequestProtocolError(codec, oversized);

    std::string malformedMetadata = valid;
    malformedMetadata[24] = 0x7f;
    malformedMetadata[25] = static_cast<char>(0xff);
    malformedMetadata[26] = static_cast<char>(0xff);
    malformedMetadata[27] = static_cast<char>(0xff);
    expectRequestProtocolError(codec, malformedMetadata);
}

void testMessageSizeLimit() {
    minirpc::RpcCodec codec;
    constexpr std::size_t metadataSize = sizeof(std::uint32_t) + 1 +
                                         sizeof(std::uint32_t) + 1;
    minirpc::RpcRequest request{
        1, "S", "M", std::string(minirpc::kMaxMessageSize - metadataSize, 'x')};
    const std::string frame = codec.encodeRequest(request);
    assert(frame.size() == minirpc::kRpcHeaderSize + minirpc::kMaxMessageSize);

    request.payload.push_back('x');
    bool rejected = false;
    try {
        (void)codec.encodeRequest(request);
    } catch (const std::length_error&) {
        rejected = true;
    }
    assert(rejected);
}

}  // namespace

int main() {
    testRequestRoundTripAndFragmentation();
    testResponseRoundTrip();
    testStickyPackets();
    testInvalidFramesAreRejectedWithoutConsumption();
    testMessageSizeLimit();
}
