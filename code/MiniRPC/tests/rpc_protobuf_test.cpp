#include "calculator.pb.h"
#include "mini_rpc/rpc_channel.h"
#include "mini_rpc/rpc_client.h"
#include "mini_rpc/rpc_controller.h"
#include "mini_rpc/rpc_server.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <thread>

namespace {

constexpr std::uint16_t kPort = 39188;

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

int call(minirpc::proto::CalculatorService_Stub& stub, int lhs, int rhs,
         void (minirpc::proto::CalculatorService_Stub::*method)(
             ::google::protobuf::RpcController*, const minirpc::proto::CalcRequest*,
             minirpc::proto::CalcResponse*, ::google::protobuf::Closure*)) {
    minirpc::RpcController controller;
    minirpc::proto::CalcRequest request;
    request.set_lhs(lhs);
    request.set_rhs(rhs);
    minirpc::proto::CalcResponse response;
    (stub.*method)(&controller, &request, &response, nullptr);
    assert(!controller.failed());
    return response.result();
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    std::promise<minireactor::EventLoop*> serverReady;
    auto serverReadyFuture = serverReady.get_future();
    CalculatorServiceImpl service;
    std::thread serverThread([&serverReady, &service] {
        minireactor::EventLoop loop;
        minirpc::RpcServer server(
            &loop, minireactor::InetAddress("127.0.0.1", kPort), 1, 2, 16);
        assert(server.registerService(&service));
        server.start();
        serverReady.set_value(&loop);
        loop.loop();
    });

    assert(serverReadyFuture.wait_for(2s) == std::future_status::ready);
    minireactor::EventLoop* serverLoop = serverReadyFuture.get();

    auto client = std::make_shared<minirpc::RpcClient>("127.0.0.1", kPort);
    assert(client->connect(2s));

    minirpc::RpcChannel channel(client);
    minirpc::proto::CalculatorService_Stub stub(&channel);
    assert(call(stub, 10, 20, &minirpc::proto::CalculatorService_Stub::Add) == 30);
    assert(call(stub, 10, 20, &minirpc::proto::CalculatorService_Stub::Sub) == -10);
    assert(call(stub, 10, 20, &minirpc::proto::CalculatorService_Stub::Mul) == 200);
    assert(call(stub, 20, 10, &minirpc::proto::CalculatorService_Stub::Div) == 2);

    minirpc::RpcController divZero;
    minirpc::proto::CalcRequest request;
    request.set_lhs(10);
    request.set_rhs(0);
    minirpc::proto::CalcResponse response;
    stub.Div(&divZero, &request, &response, nullptr);
    assert(divZero.failed());

    client->disconnect();
    client.reset();
    serverLoop->quit();
    serverThread.join();
}
