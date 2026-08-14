#include "mini_rpc/codec.h"

#include "net/Buffer.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace minirpc {
namespace {

void appendUint16(std::string& output, std::uint16_t value) {
    output.push_back(static_cast<char>((value >> 8U) & 0xffU));
    output.push_back(static_cast<char>(value & 0xffU));
}

void appendUint32(std::string& output, std::uint32_t value) {
    output.push_back(static_cast<char>((value >> 24U) & 0xffU));
    output.push_back(static_cast<char>((value >> 16U) & 0xffU));
    output.push_back(static_cast<char>((value >> 8U) & 0xffU));
    output.push_back(static_cast<char>(value & 0xffU));
}

void appendUint64(std::string& output, std::uint64_t value) {
    appendUint32(output, static_cast<std::uint32_t>(value >> 32U));
    appendUint32(output, static_cast<std::uint32_t>(value & 0xffffffffULL));
}

std::uint16_t readUint16(const char* data) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) |
                                      static_cast<std::uint16_t>(bytes[1]));
}

std::uint32_t readUint32(const char* data) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

std::uint64_t readUint64(const char* data) {
    return (static_cast<std::uint64_t>(readUint32(data)) << 32U) |
           static_cast<std::uint64_t>(readUint32(data + 4));
}

void checkStringLength(std::size_t length, const char* field) {
    if (length > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error(std::string(field) + " is too long");
    }
}

std::string encodeFrame(MessageType type, std::uint64_t requestId,
                        const std::string& metadata, const std::string& body) {
    checkStringLength(metadata.size(), "metadata");
    checkStringLength(body.size(), "body");
    if (metadata.size() > kMaxMessageSize || body.size() > kMaxMessageSize - metadata.size()) {
        throw std::length_error("RPC message exceeds the size limit");
    }

    std::string frame;
    frame.reserve(kRpcHeaderSize + metadata.size() + body.size());
    appendUint32(frame, kRpcMagic);
    appendUint16(frame, kProtocolVersion);
    appendUint16(frame, static_cast<std::uint16_t>(type));
    appendUint64(frame, requestId);
    appendUint32(frame, static_cast<std::uint32_t>(metadata.size()));
    appendUint32(frame, static_cast<std::uint32_t>(body.size()));
    frame.append(metadata);
    frame.append(body);
    return frame;
}

struct ParsedFrame {
    std::uint64_t requestId{0};
    std::string_view metadata;
    std::string_view body;
    std::size_t totalLength{0};
};

DecodeStatus parseFrame(const minireactor::Buffer& buffer, MessageType expectedType,
                        ParsedFrame& frame) {
    if (buffer.readableBytes() < kRpcHeaderSize) {
        return DecodeStatus::NeedMoreData;
    }

    const char* data = buffer.peek();
    const std::uint32_t magic = readUint32(data);
    const std::uint16_t version = readUint16(data + 4);
    const std::uint16_t type = readUint16(data + 6);
    const std::uint64_t requestId = readUint64(data + 8);
    const std::uint32_t metadataLength = readUint32(data + 16);
    const std::uint32_t bodyLength = readUint32(data + 20);

    if (magic != kRpcMagic || version != kProtocolVersion ||
        type != static_cast<std::uint16_t>(expectedType)) {
        return DecodeStatus::ProtocolError;
    }
    if (metadataLength > kMaxMessageSize ||
        bodyLength > kMaxMessageSize - static_cast<std::size_t>(metadataLength)) {
        return DecodeStatus::ProtocolError;
    }

    const std::size_t contentLength =
        static_cast<std::size_t>(metadataLength) + static_cast<std::size_t>(bodyLength);
    const std::size_t totalLength = kRpcHeaderSize + contentLength;
    if (buffer.readableBytes() < totalLength) {
        return DecodeStatus::NeedMoreData;
    }

    frame.requestId = requestId;
    frame.metadata = std::string_view(data + kRpcHeaderSize, metadataLength);
    frame.body = std::string_view(data + kRpcHeaderSize + metadataLength, bodyLength);
    frame.totalLength = totalLength;
    return DecodeStatus::Complete;
}

bool readMetadataUint32(std::string_view metadata, std::size_t& offset,
                        std::uint32_t& value) {
    if (offset > metadata.size() || metadata.size() - offset < sizeof(std::uint32_t)) {
        return false;
    }
    value = readUint32(metadata.data() + offset);
    offset += sizeof(std::uint32_t);
    return true;
}

bool readMetadataString(std::string_view metadata, std::size_t& offset,
                        std::uint32_t length, std::string& value) {
    if (offset > metadata.size() || length > metadata.size() - offset) {
        return false;
    }
    value.assign(metadata.data() + offset, length);
    offset += length;
    return true;
}

}  // namespace

std::string RpcCodec::encodeRequest(const RpcRequest& request) const {
    checkStringLength(request.service_name.size(), "service name");
    checkStringLength(request.method_name.size(), "method name");

    std::string metadata;
    metadata.reserve(sizeof(std::uint32_t) + request.service_name.size() +
                     sizeof(std::uint32_t) + request.method_name.size());
    appendUint32(metadata, static_cast<std::uint32_t>(request.service_name.size()));
    metadata.append(request.service_name);
    appendUint32(metadata, static_cast<std::uint32_t>(request.method_name.size()));
    metadata.append(request.method_name);
    return encodeFrame(MessageType::Request, request.request_id, metadata, request.payload);
}

std::string RpcCodec::encodeResponse(const RpcResponse& response) const {
    checkStringLength(response.error_message.size(), "error message");

    std::string metadata;
    metadata.reserve(sizeof(std::uint32_t) * 2 + response.error_message.size());
    appendUint32(metadata, static_cast<std::uint32_t>(response.error_code));
    appendUint32(metadata, static_cast<std::uint32_t>(response.error_message.size()));
    metadata.append(response.error_message);
    return encodeFrame(MessageType::Response, response.request_id, metadata, response.payload);
}

DecodeStatus RpcCodec::tryDecodeRequest(minireactor::Buffer& buffer,
                                        RpcRequest& request) const {
    ParsedFrame frame;
    const DecodeStatus status = parseFrame(buffer, MessageType::Request, frame);
    if (status != DecodeStatus::Complete) {
        return status;
    }

    std::size_t offset = 0;
    std::uint32_t serviceLength = 0;
    std::uint32_t methodLength = 0;
    RpcRequest decoded;
    decoded.request_id = frame.requestId;
    if (!readMetadataUint32(frame.metadata, offset, serviceLength) ||
        !readMetadataString(frame.metadata, offset, serviceLength, decoded.service_name) ||
        !readMetadataUint32(frame.metadata, offset, methodLength) ||
        !readMetadataString(frame.metadata, offset, methodLength, decoded.method_name) ||
        offset != frame.metadata.size()) {
        return DecodeStatus::ProtocolError;
    }
    decoded.payload.assign(frame.body.data(), frame.body.size());
    buffer.retrieve(frame.totalLength);
    request = std::move(decoded);
    return DecodeStatus::Complete;
}

DecodeStatus RpcCodec::tryDecodeResponse(minireactor::Buffer& buffer,
                                         RpcResponse& response) const {
    ParsedFrame frame;
    const DecodeStatus status = parseFrame(buffer, MessageType::Response, frame);
    if (status != DecodeStatus::Complete) {
        return status;
    }

    std::size_t offset = 0;
    std::uint32_t errorCode = 0;
    std::uint32_t errorMessageLength = 0;
    RpcResponse decoded;
    decoded.request_id = frame.requestId;
    if (!readMetadataUint32(frame.metadata, offset, errorCode) ||
        !readMetadataUint32(frame.metadata, offset, errorMessageLength) ||
        !readMetadataString(frame.metadata, offset, errorMessageLength,
                            decoded.error_message) ||
        offset != frame.metadata.size()) {
        return DecodeStatus::ProtocolError;
    }
    decoded.error_code = static_cast<int>(errorCode);
    decoded.payload.assign(frame.body.data(), frame.body.size());
    buffer.retrieve(frame.totalLength);
    response = std::move(decoded);
    return DecodeStatus::Complete;
}

}  // namespace minirpc
