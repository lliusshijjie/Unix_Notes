#include "net/Buffer.h"

#include <cassert>

int main() {
    minireactor::Buffer buffer;
    buffer.append("hello", 5);
    buffer.append(" world");
    assert(buffer.readableBytes() == 11);
    buffer.retrieve(6);
    assert(buffer.retrieveAllAsString() == "world");
    assert(buffer.readableBytes() == 0);
}
