#pragma once

#include "base/NonCopyable.h"

#include <cstdint>
#include <netinet/in.h>

namespace minireactor {

class Socket : private NonCopyable {
public:
    explicit Socket(int fd);
    ~Socket();

    static int createNonblocking();
    int fd() const;
    void setReuseAddr(bool enabled);
    void bindAddress(std::uint16_t port, const char* ipAddress = "0.0.0.0");
    void listen();
    int accept(sockaddr_in* peerAddress) const;
    void shutdownWrite() const;

private:
    const int fd_;
};

}  // namespace minireactor
