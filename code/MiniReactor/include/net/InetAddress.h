#pragma once

#include <cstdint>
#include <netinet/in.h>
#include <string>

namespace minireactor {

class InetAddress {
public:
    InetAddress(std::string ip, std::uint16_t port);

    const sockaddr_in& sockaddr() const noexcept;
    std::string ip() const;
    std::uint16_t port() const noexcept;
    std::string toIpPort() const;

private:
    sockaddr_in address_{};
};

}  // namespace minireactor
