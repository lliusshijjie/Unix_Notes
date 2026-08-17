#include "mini_rpc/rpc_call_options.h"
#include "mini_rpc/rpc_client.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    using namespace std::chrono_literals;

    if (argc != 3) {
        std::cerr << "usage: async_calculator_client <host> <port>\n";
        return 1;
    }

    const std::string host = argv[1];
    const auto port = static_cast<std::uint16_t>(std::stoi(argv[2]));

    minirpc::RpcClient client(host, port);
    if (!client.connect(3s)) {
        std::cerr << "failed to connect\n";
        return 1;
    }

    minirpc::RpcRequest request;
    request.service_name = "CalculatorService";
    request.method_name = "Add";
    request.payload = "10 20";

    minirpc::RpcCallOptions options;
    options.timeout = 3s;
    options.trace_id = "async-calculator-demo";

    minirpc::RpcFuture future = client.callAsync(std::move(request), options);
    const minirpc::RpcResponse response = future.get();
    client.disconnect();

    if (response.error_code != 0) {
        std::cerr << "RPC failed: " << response.error_message << "\n";
        return 1;
    }

    std::cout << "10 + 20 = " << response.payload << "\n";
    return 0;
}
