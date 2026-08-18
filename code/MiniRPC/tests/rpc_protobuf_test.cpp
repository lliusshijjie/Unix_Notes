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

class CalculatorServiceImpl : public minirpc::proto::CalculatorService {
public:
    void Add(::google::protobuf::RpcController*,
             const minirpc::proto::AddRequest* request,
             minirpc::proto::AddResponse* response,
             ::google::protobuf::Closure* done) override {
        response->set_result(request->lhs() + request->rhs());
        if (done != nullptr) {
            done->Run();
        }
    }
};

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
    minirpc::RpcController controller;
    minirpc::proto::AddRequest request;
    request.set_lhs(10);
    request.set_rhs(20);
    minirpc::proto::AddResponse response;
    stub.Add(&controller, &request, &response, nullptr);
    assert(!controller.failed());
    assert(response.result() == 30);

    client->disconnect();
    client.reset();
    serverLoop->quit();
    serverThread.join();
}
