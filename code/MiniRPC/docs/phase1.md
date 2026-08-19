# MiniRPC 第一版实现文档

## 1. 项目目标

MiniRPC 第一版的目标不是实现完整的 gRPC/brpc，而是在已经完成的 MiniReactor 基础上，实现一个**最小可运行 RPC 调用闭环**。

最终完成下面这条链路：

```text
Client
  ↓
RpcClient / RpcChannel
  ↓
Protocol + Codec
  ↓
MiniReactor TCP
  ↓
RpcServer
  ↓
ServiceRegistry
  ↓
业务方法
  ↓
RpcResponse
  ↓
Client
```

最终应该能够写出类似：

```cpp
RpcClient client("127.0.0.1", 8080);

RpcRequest request;
request.service_name = "CalculatorService";
request.method_name = "Add";
request.payload = "...";

RpcResponse response = client.call(request);
```

服务端能够根据：

```text
CalculatorService.Add
```

找到对应 C++ 方法执行，并将结果返回客户端。

---

# 2. 第一版实现范围

第一版实现以下核心模块：

```text
Protocol
Codec
ServiceRegistry
RpcServer
RpcClient
RpcController
RpcChannel
```

同时复用已经实现的：

```text
MiniReactor
ThreadPool
AsyncLogger
```

第一版重点解决：

```text
RPC 请求如何表示
TCP 如何区分完整 RPC 消息
客户端如何发送请求
服务端如何找到业务方法
响应如何与请求匹配
调用失败如何表达
```

暂时不考虑服务发现、负载均衡、重试、熔断等高级功能。

---

# 3. 整体架构

```text
┌──────────────────────── Client ────────────────────────┐

                    Business Code
                          │
                          ▼
                     RpcChannel
                          │
                          ▼
                      RpcClient
                          │
                generate request_id
                          │
                          ▼
                       Codec
                          │
                  encode RpcRequest
                          │
                          ▼
                    MiniReactor
                          │
                          │ TCP
                          ▼

└─────────────────────────────────────────────────────────┘


┌──────────────────────── Server ────────────────────────┐

                    MiniReactor
                          │
                          ▼
                       Codec
                          │
                  decode RpcRequest
                          │
                          ▼
                     RpcServer
                          │
                          ▼
                  ServiceRegistry
                          │
               service + method lookup
                          │
                          ▼
                     ThreadPool
                          │
                          ▼
                    Business Method
                          │
                          ▼
                    RpcResponse
                          │
                          ▼
                       Codec
                          │
                          ▼
                    MiniReactor

└─────────────────────────────────────────────────────────┘
```

---

# 4. 目录结构

建议：

```text
MiniRPC/
├── CMakeLists.txt
│
├── include/
│   └── mini_rpc/
│       ├── protocol.h
│       ├── codec.h
│       ├── service_registry.h
│       ├── rpc_server.h
│       ├── rpc_client.h
│       ├── rpc_controller.h
│       └── rpc_channel.h
│
├── src/
│   ├── protocol.cpp
│   ├── codec.cpp
│   ├── service_registry.cpp
│   ├── rpc_server.cpp
│   ├── rpc_client.cpp
│   ├── rpc_controller.cpp
│   └── rpc_channel.cpp
│
├── examples/
│   ├── calculator_server.cpp
│   └── calculator_client.cpp
│
└── tests/
    ├── codec_test.cpp
    ├── registry_test.cpp
    └── rpc_test.cpp
```

MiniRPC 依赖：

```text
MiniRPC
   │
   ├── MiniReactor
   ├── ThreadPool
   └── AsyncLogger
```

---

# 5. Protocol

`Protocol` 负责定义：

> 一条 RPC 消息应该长什么样。

第一版使用简单的 Length-Field-Based 二进制协议。

## 5.1 消息格式

推荐：

```text
+----------+
| Magic    | 4 bytes
+----------+
| Version  | 2 bytes
+----------+
| Type     | 2 bytes
+----------+
| RequestId| 8 bytes
+----------+
| MetaLen  | 4 bytes
+----------+
| BodyLen  | 4 bytes
+----------+
| Metadata | variable
+----------+
| Body     | variable
+----------+
```

协议头：

```cpp
struct RpcHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t message_type;

    std::uint64_t request_id;

    std::uint32_t metadata_length;
    std::uint32_t body_length;
};
```

建议：

```cpp
constexpr std::uint32_t kRpcMagic = 0x4D525043; // "MRPC"
constexpr std::uint16_t kProtocolVersion = 1;
```

消息类型：

```cpp
enum class MessageType : std::uint16_t {
    Request = 1,
    Response = 2
};
```

---

# 6. RpcRequest 与 RpcResponse

第一版定义：

```cpp
struct RpcRequest {
    std::uint64_t request_id{0};

    std::string service_name;
    std::string method_name;

    std::string payload;
};
```

响应：

```cpp
struct RpcResponse {
    std::uint64_t request_id{0};

    int error_code{0};
    std::string error_message;

    std::string payload;
};
```

其中：

```text
request_id
```

非常重要。

因为同一条 TCP 连接可能出现：

```text
Request 100
Request 101
Request 102
```

响应顺序不一定完全等于请求顺序。

客户端通过：

```text
request_id
```

找到对应等待者。

---

# 7. Metadata

第一版可以自己设计简单 Metadata。

Request Metadata：

```text
service_name length
service_name

method_name length
method_name
```

Response Metadata：

```text
error_code
error_message length
error_message
```

Body：

```text
payload
```

第一版不强制使用 Protobuf。

可以先使用：

```text
std::string payload
```

这样能够优先把 RPC 整体链路跑通。

---

# 8. Codec

Codec 负责：

```text
RpcRequest / RpcResponse
        ⇅
TCP Byte Stream
```

核心接口：

```cpp
class RpcCodec {
public:
    std::string encodeRequest(
        const RpcRequest& request);

    std::string encodeResponse(
        const RpcResponse& response);

    bool tryDecodeRequest(
        Buffer& buffer,
        RpcRequest& request);

    bool tryDecodeResponse(
        Buffer& buffer,
        RpcResponse& response);
};
```

---

# 9. Codec 解包流程

TCP 是字节流，所以可能发生：

```text
半包
粘包
```

例如：

```text
Request1 + Request2
```

可能一次：

```text
recv()
```

全部收到。

也可能：

```text
Request1 Header 前半
Request1 Header 后半
Request1 Body
```

分三次收到。

因此不能：

```text
一次 read == 一个 RPC
```

正确流程：

```text
Buffer
   │
   ▼
数据是否 >= HeaderSize？
   │
   ├── 否 → 等待更多数据
   │
   ▼
解析 Header
   │
   ▼
检查 magic / version / length
   │
   ▼
Buffer 是否 >= 完整消息长度？
   │
   ├── 否 → 等待更多数据
   │
   ▼
读取 Metadata
   │
   ▼
读取 Body
   │
   ▼
构造 RpcRequest / RpcResponse
```

需要限制：

```cpp
constexpr std::size_t kMaxMessageSize =
    16 * 1024 * 1024;
```

避免恶意请求：

```text
body_length = 4GB
```

导致内存异常。

---

# 10. ServiceRegistry

`ServiceRegistry` 负责：

> 根据 service_name + method_name 找到 C++ 业务函数。

第一版定义：

```cpp
using MethodHandler =
    std::function<void(
        const RpcRequest&,
        RpcResponse&)>;
```

接口：

```cpp
class ServiceRegistry {
public:
    bool registerMethod(
        std::string service_name,
        std::string method_name,
        MethodHandler handler);

    MethodHandler* findMethod(
        std::string_view service_name,
        std::string_view method_name);

private:
    // ...
};
```

内部可以：

```cpp
std::unordered_map<
    std::string,
    std::unordered_map<
        std::string,
        MethodHandler>>
    services_;
```

例如：

```cpp
registry.registerMethod(
    "CalculatorService",
    "Add",
    [](const RpcRequest& request,
       RpcResponse& response) {
        // business logic
    });
```

请求：

```text
service = CalculatorService
method  = Add
```

即可找到该函数。

---

# 11. RpcServer

RpcServer 是服务端核心调度器。

它负责：

```text
网络
协议
服务路由
线程池
响应
```

建议接口：

```cpp
class RpcServer {
public:
    RpcServer(
        EventLoop* loop,
        const InetAddress& address,
        std::size_t worker_threads);

    bool registerMethod(
        std::string service,
        std::string method,
        MethodHandler handler);

    void start();

private:
    void onConnection(
        const TcpConnectionPtr& connection);

    void onMessage(
        const TcpConnectionPtr& connection,
        Buffer* buffer);

    void handleRequest(
        const TcpConnectionPtr& connection,
        RpcRequest request);
};
```

---

# 12. RpcServer 请求处理流程

```text
TcpConnection 收到数据
        │
        ▼
RpcServer::onMessage()
        │
        ▼
Codec::tryDecodeRequest()
        │
        ▼
得到 RpcRequest
        │
        ▼
ServiceRegistry::findMethod()
        │
        ├── 找不到
        │      ↓
        │   MethodNotFound
        │
        ▼
提交 ThreadPool
        │
        ▼
执行 MethodHandler
        │
        ▼
构造 RpcResponse
        │
        ▼
Codec::encodeResponse()
        │
        ▼
TcpConnection::send()
```

这里非常重要：

> Service Handler 不建议直接在 IO EventLoop 中执行。

因此：

```text
IO Thread
   │
   ▼
ThreadPool
   │
   ▼
Business Logic
```

执行完成后：

```cpp
connection->send(response);
```

由 MiniReactor 自动投递回对应 EventLoop。

---

# 13. RpcClient

RpcClient 负责：

```text
建立连接
生成 request_id
发送请求
保存 PendingCall
接收响应
根据 request_id 匹配
```

核心接口：

```cpp
class RpcClient {
public:
    RpcClient(
        EventLoop* loop,
        const InetAddress& server_address);

    void connect();

    RpcResponse call(
        RpcRequest request);

private:
    void onMessage(
        const TcpConnectionPtr& connection,
        Buffer* buffer);
};
```

---

# 14. PendingCall

客户端必须维护：

```text
request_id → 等待中的 RPC
```

第一版同步 RPC 可以：

```cpp
struct PendingCall {
    std::mutex mutex;
    std::condition_variable cv;

    bool completed{false};

    RpcResponse response;
};
```

RpcClient：

```cpp
std::unordered_map<
    std::uint64_t,
    std::shared_ptr<PendingCall>>
pending_calls_;
```

请求流程：

```text
call()
  │
  ▼
request_id = next_id++
  │
  ▼
创建 PendingCall
  │
  ▼
pending_calls[request_id] = call
  │
  ▼
发送请求
  │
  ▼
等待 condition_variable
```

收到响应：

```text
response.request_id
       │
       ▼
pending_calls.find(id)
       │
       ▼
写入 response
       │
       ▼
completed = true
       │
       ▼
cv.notify_one()
```

---

# 15. RpcController

第一版 RpcController 保持简单。

它用于保存一次 RPC 调用状态。

```cpp
class RpcController {
public:
    void reset();

    bool failed() const noexcept;

    int errorCode() const noexcept;

    const std::string& errorText() const noexcept;

    void setFailed(
        int error_code,
        std::string error_message);

    void setTimeout(
        std::chrono::milliseconds timeout);

private:
    bool failed_{false};

    int error_code_{0};
    std::string error_message_;

    std::chrono::milliseconds timeout_{3000};
};
```

第一版至少支持：

```text
成功
网络错误
协议错误
ServiceNotFound
MethodNotFound
ServerError
Timeout
```

错误码可以定义：

```cpp
enum class RpcErrorCode {
    Ok = 0,

    NetworkError = 1001,
    ProtocolError = 1002,
    Timeout = 1003,

    ServiceNotFound = 2001,
    MethodNotFound = 2002,

    ServerError = 3001
};
```

---

# 16. RpcChannel

第一版可以把 RpcChannel 设计成 RpcClient 的上层封装。

例如：

```cpp
class RpcChannel {
public:
    explicit RpcChannel(
        std::shared_ptr<RpcClient> client);

    bool callMethod(
        std::string_view service,
        std::string_view method,
        std::string request_payload,
        std::string& response_payload,
        RpcController& controller);
};
```

业务调用：

```cpp
RpcController controller;

std::string response;

bool ok = channel.callMethod(
    "CalculatorService",
    "Add",
    request,
    response,
    controller);
```

内部：

```text
构造 RpcRequest
      ↓
RpcClient::call()
      ↓
检查 RpcResponse
      ↓
填充 RpcController
      ↓
返回 payload
```

第一版不需要立刻继承：

```cpp
google::protobuf::RpcChannel
```

等基本 RPC 跑通后，再考虑 Protobuf 集成。

---

# 17. 完整请求流程

假设客户端调用：

```text
CalculatorService.Add(10, 20)
```

流程：

```text
Client Business Code
        │
        ▼
RpcChannel::callMethod()
        │
        ▼
RpcClient::call()
        │
        ▼
request_id = 1001
        │
        ▼
RpcRequest
{
    id      = 1001
    service = CalculatorService
    method  = Add
    payload = ...
}
        │
        ▼
Codec Encode
        │
        ▼
MiniReactor
        │
        │ TCP
        ▼
RpcServer
        │
        ▼
Codec Decode
        │
        ▼
ServiceRegistry
        │
        ▼
CalculatorService.Add
        │
        ▼
ThreadPool
        │
        ▼
result = 30
        │
        ▼
RpcResponse
{
    id      = 1001
    code    = 0
    payload = 30
}
        │
        ▼
Codec Encode
        │
        ▼
TCP
        │
        ▼
RpcClient
        │
        ▼
pending_calls[1001]
        │
        ▼
唤醒调用线程
        │
        ▼
return response
```

---

# 18. 线程模型

建议：

```text
Main Reactor Thread
        │
        └── accept

IO Reactor Threads
        │
        ├── read RPC
        ├── decode
        └── send response

Business ThreadPool
        │
        └── execute MethodHandler

Client Calling Thread
        │
        └── wait PendingCall
```

核心原则：

```text
网络 IO
    → Reactor Thread

业务逻辑
    → ThreadPool

同步等待
    → Caller Thread
```

不能让：

```text
EventLoop Thread
```

阻塞等待 RPC 结果。

---

# 19. 实现顺序

推荐严格按照：

```text
1. Protocol
      ↓
2. Codec
      ↓
3. Codec 单元测试
      ↓
4. ServiceRegistry
      ↓
5. RpcServer
      ↓
6. RpcClient
      ↓
7. PendingCall + request_id
      ↓
8. RpcController
      ↓
9. RpcChannel
      ↓
10. CalculatorService
      ↓
11. 端到端测试
```

其中最关键节点是：

```text
RpcClient
    ↓
RpcServer
    ↓
ServiceRegistry
    ↓
RpcResponse
    ↓
RpcClient
```

这条闭环跑通以后，第一版 MiniRPC 的核心实际上就已经完成。

---

# 20. 第一版暂不实现

以下内容全部留到第二版以后：

```text
Protobuf Stub 自动生成
服务发现
注册中心
负载均衡
连接池
自动重连
自动重试
熔断
限流
流式 RPC
压缩
TLS
Tracing
Metrics
认证
多协议支持
HTTP/2
协程 RPC
异步 Future API
```

第一版不要因为这些功能把 RPC 主链路复杂化。

---

# 21. 必须测试的场景

## 正常调用

```text
CalculatorService.Add
```

正确返回结果。

## Service 不存在

请求：

```text
UnknownService.Add
```

返回：

```text
ServiceNotFound
```

## Method 不存在

请求：

```text
CalculatorService.Unknown
```

返回：

```text
MethodNotFound
```

## 半包

一次 RPC 请求拆成多次 TCP 发送。

Codec 能正常等待完整数据。

## 粘包

连续发送：

```text
Request1 Request2 Request3
```

Codec 能一次解析多个请求。

## 多请求并发

同一 RpcClient 同时发送多个请求：

```text
1001
1002
1003
```

即使响应顺序变化，也能够根据 request_id 正确匹配。

## 大消息

发送接近：

```text
kMaxMessageSize
```

的数据。

## 非法 magic

服务端正确拒绝非法协议。

## 客户端提前断开

业务线程完成后发送响应时，不得导致进程崩溃。

---

# 22. 第一版完成标准

* [ ] 自定义 RPC Protocol 完成。
* [ ] Request/Response 编码完成。
* [ ] Codec 能处理半包和粘包。
* [ ] 支持消息长度限制。
* [ ] ServiceRegistry 可以注册和查询方法。
* [ ] RpcServer 能接收并执行 RPC。
* [ ] 业务方法运行在线程池。
* [ ] RpcClient 可以发送同步 RPC。
* [ ] request_id 可以唯一标识一次请求。
* [ ] PendingCall 可以正确匹配响应。
* [ ] RpcController 可以表达错误状态。
* [ ] RpcChannel 可以封装业务调用。
* [ ] ServiceNotFound / MethodNotFound 能正确返回。
* [ ] 支持同一连接多个并发请求。
* [ ] CalculatorService Demo 正常工作。
* [ ] 通过半包、粘包、断连测试。
* [ ] ASan 未发现明显内存问题。
* [ ] TSan 未发现明显数据竞争。

---

# 23. 第一版最值得掌握的知识

完成 MiniRPC 第一版后，重点不是记住代码，而是理解以下关系：

```text
RPC ≠ 网络库
```

网络库只解决：

```text
如何可靠传输字节
```

RPC 在此之上继续解决：

```text
调用谁
传什么参数
如何编码
如何区分请求
如何找到方法
如何返回结果
如何表达错误
```

最终应该形成下面这个认识：

```text
RPC
=
Transport
+
Protocol
+
Codec
+
Request Correlation
+
Service Dispatch
+
Error Handling
```

其中：

```text
MiniReactor
```

负责 Transport；

```text
Protocol + Codec
```

负责消息边界与序列化；

```text
request_id + PendingCall
```

负责请求与响应关联；

```text
ServiceRegistry
```

负责服务方法路由；

```text
RpcController
```

负责调用状态与错误。

---

# 24. 第一版最终目标

第一版完成后，不要求它达到 brpc/gRPC 的工程水平。

只要求真正实现：

```text
业务代码
   ↓
像调用函数一样发起 RPC
   ↓
客户端编码请求
   ↓
网络发送
   ↓
服务端解析
   ↓
找到对应方法
   ↓
执行业务
   ↓
返回结果
   ↓
客户端匹配响应
```

只要这个闭环稳定工作，MiniRPC 第一版就算完成。

下一版再重点考虑：

```text
超时
异步调用
连接管理
序列化框架
服务治理
```

而不是继续往第一版堆功能。
