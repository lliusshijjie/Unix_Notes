#include "net/InetAddress.h"

#include <arpa/inet.h>
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace minireactor {

InetAddress::InetAddress(std::string ip, std::uint16_t port) {
    address_.sin_family = AF_INET;
    address_.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &address_.sin_addr) != 1) {
        throw std::invalid_argument("address must be a valid IPv4 address");
    }
}

const sockaddr_in& InetAddress::sockaddr() const noexcept {
    return address_;
}

std::string InetAddress::ip() const {
    char buffer[INET_ADDRSTRLEN]{};
    if (::inet_ntop(AF_INET, &address_.sin_addr, buffer, sizeof(buffer)) == nullptr) {
        throw std::system_error(errno, std::generic_category(), "inet_ntop");
    }
    return buffer;
}

std::uint16_t InetAddress::port() const noexcept {
    return ntohs(address_.sin_port);
}

std::string InetAddress::toIpPort() const {
    return ip() + ":" + std::to_string(port());
}

}  // namespace minireactor
