#include "net/InetAddress.h"

#include <cassert>
#include <cstdint>
#include <netinet/in.h>
#include <stdexcept>
#include <string>

int main() {
    minireactor::InetAddress address("127.0.0.1", 8080);
    assert(address.ip() == "127.0.0.1");
    assert(address.port() == 8080);
    assert(address.toIpPort() == "127.0.0.1:8080");
    assert(address.sockaddr().sin_family == AF_INET);
    assert(ntohs(address.sockaddr().sin_port) == 8080);

    bool rejected = false;
    try {
        minireactor::InetAddress invalid("300.1.1.1", 1);
        (void)invalid;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
