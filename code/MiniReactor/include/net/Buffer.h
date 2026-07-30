#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace minireactor {

class Buffer {
public:
    void append(const char* data, std::size_t length);
    void append(std::string_view data);

    const char* peek() const;
    std::size_t readableBytes() const;
    std::string retrieveAllAsString();
    void retrieve(std::size_t length);
    void retrieveAll();

private:
    std::vector<char> data_;
    std::size_t readerIndex_ = 0;
};

}  // namespace minireactor
