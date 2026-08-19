#include "calculator.pb.h"
#include "mini_rpc/rpc_server.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {

void complete(::google::protobuf::Closure* done) {
    if (done != nullptr) {
        done->Run();
    }
}

class CalculatorServiceImpl : public minirpc::proto::CalculatorService {
public:
    void Add(::google::protobuf::RpcController*,
             const minirpc::proto::CalcRequest* request,
             minirpc::proto::CalcResponse* response,
             ::google::protobuf::Closure* done) override {
        response->set_result(request->lhs() + request->rhs());
        complete(done);
    }

    void Sub(::google::protobuf::RpcController*,
             const minirpc::proto::CalcRequest* request,
             minirpc::proto::CalcResponse* response,
             ::google::protobuf::Closure* done) override {
        response->set_result(request->lhs() - request->rhs());
        complete(done);
    }

    void Mul(::google::protobuf::RpcController*,
             const minirpc::proto::CalcRequest* request,
             minirpc::proto::CalcResponse* response,
             ::google::protobuf::Closure* done) override {
        response->set_result(request->lhs() * request->rhs());
        complete(done);
    }

    void Div(::google::protobuf::RpcController* controller,
             const minirpc::proto::CalcRequest* request,
             minirpc::proto::CalcResponse* response,
             ::google::protobuf::Closure* done) override {
        if (request->rhs() == 0) {
            if (controller != nullptr) {
                controller->SetFailed("division by zero");
            }
            complete(done);
            return;
        }
        response->set_result(request->lhs() / request->rhs());
        complete(done);
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    const std::uint16_t port = static_cast<std::uint16_t>(
        argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 8080);
    try {
        minireactor::EventLoop loop;
        minirpc::RpcServer server(
            &loop, minireactor::InetAddress("0.0.0.0", port), 2, 4, 1024);
        CalculatorServiceImpl service;
        if (!server.registerService(&service)) {
            std::cerr << "failed to register CalculatorService\n";
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
