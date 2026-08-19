#include "calculator.pb.h"
#include "mini_rpc/rpc_channel.h"
#include "mini_rpc/rpc_client.h"
#include "mini_rpc/rpc_controller.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

namespace {

bool invoke(minirpc::proto::CalculatorService_Stub& stub, int lhs, int rhs,
            void (minirpc::proto::CalculatorService_Stub::*method)(
                ::google::protobuf::RpcController*, const minirpc::proto::CalcRequest*,
                minirpc::proto::CalcResponse*, ::google::protobuf::Closure*),
            const char* op) {
    minirpc::RpcController controller;
    minirpc::proto::CalcRequest request;
    request.set_lhs(lhs);
    request.set_rhs(rhs);
    minirpc::proto::CalcResponse response;
    (stub.*method)(&controller, &request, &response, nullptr);
    if (controller.failed()) {
        std::cerr << "RPC failed, code=" << controller.errorCode()
                  << ", message=" << controller.errorText() << '\n';
        return false;
    }
    std::cout << lhs << ' ' << op << ' ' << rhs << " = " << response.result() << '\n';
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string ip = argc > 1 ? argv[1] : "127.0.0.1";
    const std::uint16_t port = static_cast<std::uint16_t>(
        argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 8080);
    try {
        auto client = std::make_shared<minirpc::RpcClient>(ip, port);
        if (!client->connect(std::chrono::seconds(3))) {
            std::cerr << "failed to connect to " << ip << ':' << port << '\n';
            return 1;
        }

        minirpc::RpcChannel channel(client);
        minirpc::proto::CalculatorService_Stub stub(&channel);
        if (!invoke(stub, 10, 20, &minirpc::proto::CalculatorService_Stub::Add, "+") ||
            !invoke(stub, 10, 20, &minirpc::proto::CalculatorService_Stub::Sub, "-") ||
            !invoke(stub, 10, 20, &minirpc::proto::CalculatorService_Stub::Mul, "*") ||
            !invoke(stub, 20, 10, &minirpc::proto::CalculatorService_Stub::Div, "/")) {
            return 1;
        }
        client->disconnect();
    } catch (const std::exception& error) {
        std::cerr << "calculator client failed: " << error.what() << '\n';
        return 1;
    }
}
