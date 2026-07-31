#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace minireactor {

class Buffer {
public:
    static constexpr std::size_t kCheapPrepend = 8;
    static constexpr std::size_t kInitialSize = 1024;

    Buffer();

    void append(const char* data, std::size_t length);
    void append(std::string_view data);
    void prepend(const char* data, std::size_t length);
    void prepend(std::string_view data);

    const char* peek() const;
    std::size_t readableBytes() const;
    std::size_t writableBytes() const;
    std::size_t prependableBytes() const;
    std::string retrieveAllAsString();
    void retrieve(std::size_t length);
    void retrieveAll();

private:
    void ensureWritableBytes(std::size_t length);
    void makeSpace(std::size_t length);

    std::vector<char> data_;
    std::size_t readerIndex_ = kCheapPrepend;
    std::size_t writerIndex_ = kCheapPrepend;
};

}  // namespace minireactor
