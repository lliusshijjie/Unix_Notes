#pragma once

#include <cstdint>
#include <string>

namespace minirpc {

struct Endpoint {
    std::string host;
    std::uint16_t port{0};
};

inline bool operator==(const Endpoint& lhs, const Endpoint& rhs) {
    return lhs.host == rhs.host && lhs.port == rhs.port;
}

inline bool operator!=(const Endpoint& lhs, const Endpoint& rhs) {
    return !(lhs == rhs);
}

inline std::string toString(const Endpoint& endpoint) {
    return endpoint.host + ":" + std::to_string(endpoint.port);
}

}  // namespace minirpc
