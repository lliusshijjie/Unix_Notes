#include "mini_rpc/rpc_server.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

int main(int argc, char* argv[]) {
    const std::uint16_t port = static_cast<std::uint16_t>(
        argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 8080);
    try {
        minireactor::EventLoop loop;
        minirpc::RpcServer server(
            &loop, minireactor::InetAddress("0.0.0.0", port), 2, 4, 1024);
        if (!server.registerMethod(
                "CalculatorService", "Add",
                [](const minirpc::RpcRequest& request,
                   minirpc::RpcResponse& response) {
                    std::istringstream input(request.payload);
                    long long left = 0;
                    long long right = 0;
                    if (!(input >> left >> right)) {
                        throw std::invalid_argument("payload must contain two integers");
                    }
                    input >> std::ws;
                    if (!input.eof()) {
                        throw std::invalid_argument("payload contains trailing data");
                    }
                    response.payload = std::to_string(left + right);
                })) {
            std::cerr << "failed to register CalculatorService.Add\n";
            return 1;
        }
        server.start();
        std::cout << "CalculatorService listening on 0.0.0.0:" << port << '\n';
        loop.loop();
    } catch (const std::exception& error) {
        std::cerr << "calculator server failed: " << error.what() << '\n';
        return 1;
    }
}
