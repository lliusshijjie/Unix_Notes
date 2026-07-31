#include "net/Buffer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace minireactor {

Buffer::Buffer() : data_(kCheapPrepend + kInitialSize) {}

void Buffer::append(const char* data, std::size_t length) {
    ensureWritableBytes(length);
    std::memcpy(data_.data() + writerIndex_, data, length);
    writerIndex_ += length;
}

void Buffer::append(std::string_view data) {
    append(data.data(), data.size());
}

void Buffer::prepend(const char* data, std::size_t length) {
    if (length > prependableBytes()) {
        throw std::out_of_range("not enough prependable space in the buffer");
    }
    readerIndex_ -= length;
    std::memcpy(data_.data() + readerIndex_, data, length);
}

void Buffer::prepend(std::string_view data) {
    prepend(data.data(), data.size());
}

const char* Buffer::peek() const {
    return data_.data() + readerIndex_;
}

std::size_t Buffer::readableBytes() const {
    return writerIndex_ - readerIndex_;
}

std::size_t Buffer::writableBytes() const {
    return data_.size() - writerIndex_;
}

std::size_t Buffer::prependableBytes() const {
    return readerIndex_;
}

std::string Buffer::retrieveAllAsString() {
    std::string result(peek(), readableBytes());
    retrieveAll();
    return result;
}

void Buffer::retrieve(std::size_t length) {
    if (length > readableBytes()) {
        throw std::out_of_range("cannot retrieve more bytes than the buffer contains");
    }
    readerIndex_ += length;
    if (readerIndex_ == writerIndex_) {
        retrieveAll();
    }
}

void Buffer::retrieveAll() {
    readerIndex_ = kCheapPrepend;
    writerIndex_ = kCheapPrepend;
}

void Buffer::ensureWritableBytes(std::size_t length) {
    if (writableBytes() < length) {
        makeSpace(length);
    }
}

void Buffer::makeSpace(std::size_t length) {
    if (writableBytes() + prependableBytes() < length + kCheapPrepend) {
        data_.resize(writerIndex_ + length);
        return;
    }

    const std::size_t readable = readableBytes();
    std::copy(data_.begin() + static_cast<std::ptrdiff_t>(readerIndex_),
              data_.begin() + static_cast<std::ptrdiff_t>(writerIndex_),
              data_.begin() + static_cast<std::ptrdiff_t>(kCheapPrepend));
    readerIndex_ = kCheapPrepend;
    writerIndex_ = readerIndex_ + readable;
}

}  // namespace minireactor
