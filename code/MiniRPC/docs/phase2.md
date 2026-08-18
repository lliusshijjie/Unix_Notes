# MiniRPC Phase2：功能增强版

> 目标：在 MiniRPC 第一版“同步 RPC + 自定义 Protocol/Codec + ServiceRegistry + RpcClient/RpcServer”基础上，增加标准序列化、异步调用、超时控制、多实例发现和负载均衡。

## 1. Phase2 目标

本阶段只实现 5 个核心功能：

```text
1. Protobuf 标准序列化
2. Future 异步 RPC
3. RPC 超时控制
4. 多实例服务发现
5. Round-Robin 负载均衡
```

最终调用链：

```text
业务代码
   ↓
Protobuf Stub / RpcChannel
   ↓
RpcClient::asyncCall()
   ↓
ServiceDiscovery
   ↓
LoadBalancer
   ↓
选择一个 Server Endpoint
   ↓
MiniReactor / TCP
   ↓
RpcServer
   ↓
Protobuf Service
   ↓
返回响应
   ↓
Promise 完成
   ↓
Future 得到结果
```

## 2. 本阶段不实现

暂时不要加入：

- 自动重试
- 熔断
- 限流
- TLS
- 认证
- Tracing
- Prometheus Metrics
- HTTP/2
- 流式 RPC
- 协程 RPC
- 多协议支持
- 复杂连接池
- 一致性哈希
- etcd / Consul / ZooKeeper

Phase2 重点是先把 RPC 从“单机同步调用”升级为“支持多实例的异步 RPC”。

## 3. 总体架构

```text
                    Client
                      │
                      ▼
                Protobuf Stub
                      │
                      ▼
                 RpcChannel
                      │
                      ▼
                  RpcClient
              ┌───────┴────────┐
              ▼                ▼
       ServiceDiscovery    PendingCall
              │                │
              ▼                │
        LoadBalancer            │
              │                │
              ▼                │
        Endpoint A/B/C         Timer
              │                │
              └───────┬────────┘
                      ▼
                 MiniReactor
                      │
                     TCP
                      │
                      ▼
                  RpcServer
                      │
                      ▼
             Protobuf Service
                      │
                      ▼
                  Response
```

## 4. Protobuf 标准序列化

### 4.1 目标

第一版可能使用：

```cpp
RpcRequest {
    std::string service_name;
    std::string method_name;
    std::string payload;
};
```

Phase2 改成由 Protobuf 定义业务消息：

```proto
syntax = "proto3";

service CalculatorService {
    rpc Add(AddRequest) returns (AddResponse);
}

message AddRequest {
    int32 lhs = 1;
    int32 rhs = 2;
}

message AddResponse {
    int32 result = 1;
}
```

通过：

```text
.proto
  ↓
protoc
  ↓
Message + Service + Stub
```

生成业务接口。

### 4.2 RpcChannel

让自己的 `RpcChannel` 适配 `google::protobuf::RpcChannel`：

```cpp
void CallMethod(
    const google::protobuf::MethodDescriptor* method,
    google::protobuf::RpcController* controller,
    const google::protobuf::Message* request,
    google::protobuf::Message* response,
    google::protobuf::Closure* done) override;
```

从 `MethodDescriptor` 获取：

```text
service_name
method_name
```

请求：

```text
request->SerializeToString()
```

响应：

```text
response->ParseFromString()
```

### 4.3 服务端注册

RpcServer 支持：

```cpp
CalculatorServiceImpl service;
server.registerService(&service);
```

收到请求后：

```text
service_name + method_name
        ↓
找到 Service
        ↓
找到 MethodDescriptor
        ↓
创建 Request / Response
        ↓
CallMethod()
```

## 5. Future 异步 RPC

第一版：

```cpp
RpcResponse response = client.call(request);
```

Phase2：

```cpp
std::future<RpcResponse> future =
    client.asyncCall(request);
```

PendingCall 改为：

```cpp
struct PendingCall {
    std::promise<RpcResponse> promise;
};
```

RpcClient 保存：

```cpp
std::unordered_map<
    std::uint64_t,
    std::shared_ptr<PendingCall>>
pending_calls_;
```

调用流程：

```text
生成 request_id
    ↓
创建 promise
    ↓
取得 future
    ↓
pending_calls[id] = pending
    ↓
发送请求
    ↓
立即返回 future
```

收到响应：

```text
根据 request_id 查 PendingCall
    ↓
promise.set_value(response)
    ↓
删除 PendingCall
```

同步调用建议建立在异步调用上：

```cpp
RpcResponse RpcClient::call(RpcRequest request) {
    return asyncCall(std::move(request)).get();
}
```

## 6. RPC 超时控制

任何 RPC 都不能无限等待。

调用：

```cpp
RpcController controller;
controller.setTimeout(
    std::chrono::milliseconds(1000));
```

发送请求后：

```text
Request 1001
    ↓
pending_calls_[1001]
    ↓
注册 Timer
    ↓
等待 Response 或 Timeout
```

### 响应先到

```text
Response 1001
    ↓
取消 Timer
    ↓
promise.set_value(response)
    ↓
删除 PendingCall
```

### Timeout 先到

```text
Timer 到期
    ↓
PendingCall 仍存在
    ↓
删除 PendingCall
    ↓
设置 Timeout 错误
    ↓
完成 Promise
```

迟到响应直接丢弃，不允许重复完成 Promise。

推荐错误码：

```cpp
enum class RpcErrorCode {
    Ok = 0,
    NetworkError = 1001,
    ProtocolError = 1002,
    Timeout = 1003,
    ServiceNotFound = 2001,
    MethodNotFound = 2002,
    ServiceUnavailable = 2003,
    ServerError = 3001
};
```

## 7. 多实例服务发现

第一版：

```cpp
RpcClient("10.0.0.1", 8000);
```

Phase2：

```cpp
RpcClient("CalculatorService");
```

服务名对应多个实例：

```text
CalculatorService

├── 10.0.0.1:8000
├── 10.0.0.2:8000
└── 10.0.0.3:8000
```

Phase2 不接 etcd，先实现内存版：

```cpp
struct Endpoint {
    std::string host;
    std::uint16_t port;
};

class ServiceDiscovery {
public:
    void registerEndpoint(
        std::string service,
        Endpoint endpoint);

    void unregisterEndpoint(
        std::string_view service,
        const Endpoint& endpoint);

    std::vector<Endpoint> discover(
        std::string_view service) const;
};
```

内部可使用：

```cpp
std::unordered_map<
    std::string,
    std::vector<Endpoint>>
services_;
```

本阶段重点理解：

> 一个 Service 不再对应一个 IP，而是对应一组可用 Endpoint。

## 8. Round-Robin 负载均衡

接口：

```cpp
class LoadBalancer {
public:
    virtual ~LoadBalancer() = default;

    virtual Endpoint select(
        const std::vector<Endpoint>& endpoints) = 0;
};
```

实现：

```cpp
class RoundRobinLoadBalancer
    : public LoadBalancer {
public:
    Endpoint select(
        const std::vector<Endpoint>& endpoints) override;

private:
    std::atomic<std::size_t> index_{0};
};
```

逻辑：

```text
Request 1 → A
Request 2 → B
Request 3 → C
Request 4 → A
```

核心：

```cpp
auto index =
    index_.fetch_add(1, std::memory_order_relaxed);

return endpoints[index % endpoints.size()];
```

必须处理：

```text
endpoints.empty()
```

此时快速返回 `ServiceUnavailable`。

## 9. RpcClient 调用链升级

Phase2：

```text
RpcClient::asyncCall()
        ↓
根据 service_name 调用 discover()
        ↓
得到 Endpoint 列表
        ↓
LoadBalancer::select()
        ↓
选择目标 Server
        ↓
复用对应 TCP 长连接
        ↓
创建 PendingCall
        ↓
注册 Timeout
        ↓
发送 Request
        ↓
返回 Future
```

## 10. 多实例连接管理

Phase2 暂时采用：

```text
一个 Endpoint 对应一个长期 TcpConnection
```

例如：

```text
127.0.0.1:8001 → RpcConnection A
127.0.0.1:8002 → RpcConnection B
127.0.0.1:8003 → RpcConnection C
```

不做复杂连接池，只需要尽量复用已有长连接。

## 11. 建议新增文件

```text
MiniRPC/
├── include/
│   └── mini_rpc/
│       ├── endpoint.h
│       ├── service_discovery.h
│       ├── load_balancer.h
│       └── round_robin_load_balancer.h
├── src/
│   ├── service_discovery.cpp
│   └── round_robin_load_balancer.cpp
├── proto/
│   └── calculator.proto
└── examples/
    ├── calculator_server.cpp
    ├── calculator_server_2.cpp
    └── calculator_client.cpp
```

原有模块需要重点修改：

```text
RpcClient
RpcServer
RpcChannel
RpcController
ServiceRegistry
PendingCall
```

## 12. 推荐实现顺序

```text
1. 接入 Protobuf
      ↓
2. RpcChannel 适配 Protobuf
      ↓
3. RpcServer 注册 Protobuf Service
      ↓
4. Future + Promise 异步调用
      ↓
5. 同步 call() 建立在 asyncCall() 上
      ↓
6. Timeout
      ↓
7. Endpoint
      ↓
8. ServiceDiscovery
      ↓
9. RoundRobinLoadBalancer
      ↓
10. RpcClient 多实例调用
      ↓
11. 多 Server 联调
```

先确保：

```text
Protobuf + asyncCall + timeout
```

在单 Server 下稳定工作，再扩展多实例。

## 13. 核心测试

### Protobuf 调用

```text
Add(10, 20) → 30
```

### Future 异步 RPC

同时发起 100 个 RPC，先取得 100 个 Future，再等待结果。

要求：

- request_id 不冲突
- 响应正确匹配
- 不要求响应按请求顺序返回

### Timeout

Server 延迟 2 秒，Client Timeout 设置 500ms。

要求：

```text
约 500ms 返回 Timeout
```

迟到响应不得重复完成 Promise。

### 多实例发现

启动：

```text
Server A : 8001
Server B : 8002
Server C : 8003
```

`discover("CalculatorService")` 应得到 3 个 Endpoint。

### Round-Robin

连续调用：

```text
1 → A
2 → B
3 → C
4 → A
```

测试时可让不同 Server 返回自己的 `server_id`。

### 无可用实例

如果服务没有 Endpoint：

```text
立即返回 ServiceUnavailable
```

不能无限等待。

## 14. Phase2 完成标准

- [ ] `.proto` 可以生成 Message / Service / Stub。
- [ ] RpcChannel 适配 Protobuf RPC。
- [ ] RpcServer 可以注册 Protobuf Service。
- [ ] Request / Response 使用 Protobuf 序列化。
- [ ] `asyncCall()` 返回 `std::future`。
- [ ] PendingCall 使用 Promise/Future。
- [ ] 同步 RPC 建立在异步 RPC 之上。
- [ ] 每次 RPC 支持 Timeout。
- [ ] Timeout 后 PendingCall 正确清理。
- [ ] 迟到 Response 不会重复完成 Promise。
- [ ] ServiceDiscovery 支持一个服务多个 Endpoint。
- [ ] Round-Robin 负载均衡完成。
- [ ] RpcClient 可以选择不同 Server 实例。
- [ ] Endpoint 对应连接可以复用。
- [ ] 三实例联调通过。
- [ ] ASan 无明显内存错误。
- [ ] TSan 无明显数据竞争。

## 15. Phase2 最终能力

```text
MiniRPC V1
同步 RPC
+
自定义协议
+
固定 Server
```

升级为：

```text
MiniRPC V2
Protobuf 标准序列化
+
Future 异步 RPC
+
Timeout
+
Service Discovery
+
Load Balancing
```

这五项功能分别解决：

```text
Protobuf
→ 数据怎么标准化描述和序列化

Future
→ RPC 如何异步执行

Timeout
→ 请求如何避免无限等待

ServiceDiscovery
→ 服务实例在哪里

LoadBalancer
→ 本次请求应该选择哪台实例
```

完成这五项后，MiniRPC Phase2 即可收尾，不继续加入重试、熔断、TLS、HTTP/2、Streaming、Coroutine 等能力。
