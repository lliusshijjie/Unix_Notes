#include "net/Buffer.h"

#include <cassert>

int main() {
    minireactor::Buffer buffer;
    assert(buffer.prependableBytes() == minireactor::Buffer::kCheapPrepend);
    buffer.append("hello", 5);
    buffer.append(" world");
    assert(buffer.readableBytes() == 11);
    buffer.retrieve(6);
    assert(buffer.retrieveAllAsString() == "world");
    assert(buffer.readableBytes() == 0);

    buffer.append("first payload");
    buffer.retrieve(6);
    buffer.append(" and second payload that reuses the prependable area");
    assert(buffer.retrieveAllAsString() ==
           "payload and second payload that reuses the prependable area");

    buffer.append("body");
    buffer.prepend("head", 4);
    assert(buffer.retrieveAllAsString() == "headbody");
}
