#include "net/Buffer.h"

#include <algorithm>
#include <stdexcept>

namespace minireactor {

void Buffer::append(const char* data, std::size_t length) {
    data_.insert(data_.end(), data, data + length);
}

void Buffer::append(std::string_view data) {
    append(data.data(), data.size());
}

const char* Buffer::peek() const {
    return data_.data() + readerIndex_;
}

std::size_t Buffer::readableBytes() const {
    return data_.size() - readerIndex_;
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
    if (readerIndex_ == data_.size()) {
        retrieveAll();
    }
}

void Buffer::retrieveAll() {
    data_.clear();
    readerIndex_ = 0;
}

}  // namespace minireactor
