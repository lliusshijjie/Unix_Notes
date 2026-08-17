# MiniRPC 第二版实现文档

## 1. 项目目标

MiniRPC 第二版的目标不是立刻实现完整的服务治理体系，而是在第一版已经跑通
同步 RPC 闭环的基础上，把客户端调用模型和连接生命周期补扎实。

第一版解决的是：

```text
一次 RPC 如何从客户端发到服务端，再把响应返回客户端
```

第二版重点解决的是：

```text
RPC 调用如何在真实客户端中长期、并发、非阻塞地运行
```

最终应该能够写出类似：

```cpp
RpcClient client("127.0.0.1", 8080);
client.connect();

RpcRequest request;
request.service_name = "CalculatorService";
request.method_name = "Add";
request.payload = "10 20";

auto future = client.callAsync(request);

// caller 可以继续做其他事情

RpcResponse response = future.get();
```

也可以写出回调式调用：

```cpp
client.callAsync(
    request,
    [](RpcResponse response) {
        // handle response
    });
```

同时，客户端应该能够在连接断开后自动重连，新请求按照明确策略处理，旧请求
按照明确错误返回，不再依赖调用者手动判断所有连接边界。

---

# 2. 第二版实现范围

第二版实现以下核心能力：

```text
RpcFuture / RpcCallback
RpcCallOptions
Deadline / Cancellation
ClientConnectionState
AutoReconnect
RetryPolicy
Serializer 接口
RpcMetrics
RpcTraceContext
```

同时继续复用：

```text
MiniReactor
TcpClient / Connector
ThreadPool
AsyncLogger
Protocol + Codec
ServiceRegistry
RpcServer
RpcClient
RpcController
RpcChannel
```

第二版重点解决：

```text
客户端如何发起非阻塞 RPC
PendingCall 如何统一完成、超时和取消
连接断开后客户端如何恢复
重连期间请求如何处理
调用超时如何成为 RPC 语义
业务 payload 如何逐步接入序列化框架
RPC 调用如何记录基础指标和追踪信息
```

第二版仍然不把服务发现、注册中心、负载均衡、TLS、HTTP/2 作为主线目标。

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
              ┌───────────┴───────────┐
              │                       │
              ▼                       ▼
          RpcCallOptions          PendingCallMap
              │                       │
              ▼                       ▼
        Deadline / Cancel        RpcFuture / Callback
              │                       │
              └───────────┬───────────┘
                          ▼
                     RpcCodec
                          │
                          ▼
                    MiniReactor TCP
                          │
                          ▼
                 ClientConnectionState
                          │
                          ▼
                    AutoReconnect

└─────────────────────────────────────────────────────────┘


┌──────────────────────── Server ────────────────────────┐

                    MiniReactor TCP
                          │
                          ▼
                     RpcCodec
                          │
                          ▼
                     RpcServer
                          │
                          ▼
                 RpcTraceContext
                          │
                          ▼
                  ServiceRegistry
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
                    RpcMetrics

└─────────────────────────────────────────────────────────┘
```

第二版仍然保持：

```text
Transport 由 MiniReactor 负责
Protocol + Codec 负责消息边界
RpcClient 负责请求关联和连接管理
RpcServer 负责服务路由和业务调度
```

但第一版中的同步等待不再是唯一调用方式。

---

# 4. 目录结构

建议在第一版目录结构上增量扩展：

```text
MiniRPC/
├── include/
│   └── mini_rpc/
│       ├── protocol.h
│       ├── codec.h
│       ├── service_registry.h
│       ├── rpc_server.h
│       ├── rpc_client.h
│       ├── rpc_controller.h
│       ├── rpc_channel.h
│       ├── rpc_call_options.h
│       ├── rpc_future.h
│       ├── rpc_serializer.h
│       ├── rpc_metrics.h
│       └── rpc_trace_context.h
│
├── src/
│   ├── protocol.cpp
│   ├── codec.cpp
│   ├── service_registry.cpp
│   ├── rpc_server.cpp
│   ├── rpc_client.cpp
│   ├── rpc_controller.cpp
│   ├── rpc_channel.cpp
│   ├── rpc_call_options.cpp
│   ├── rpc_future.cpp
│   ├── rpc_serializer.cpp
│   ├── rpc_metrics.cpp
│   └── rpc_trace_context.cpp
│
├── examples/
│   ├── calculator_server.cpp
│   ├── calculator_client.cpp
│   └── async_calculator_client.cpp
│
└── tests/
    ├── codec_test.cpp
    ├── registry_test.cpp
    ├── rpc_server_test.cpp
    ├── rpc_client_test.cpp
    ├── rpc_channel_test.cpp
    ├── rpc_async_client_test.cpp
    ├── rpc_deadline_test.cpp
    ├── rpc_reconnect_test.cpp
    ├── rpc_serializer_test.cpp
    └── rpc_metrics_test.cpp
```

第二版应该优先改造 `RpcClient`，再扩展 `RpcChannel`。不要先改
`RpcServer` 的服务注册模型。

---

# 5. RpcCallOptions

`RpcCallOptions` 负责描述一次调用的行为。

第一版的 timeout 是 `RpcClient::call()` 的参数，第二版应该把它变成 RPC
调用的一部分。

建议：

```cpp
struct RpcCallOptions {
    std::chrono::milliseconds timeout{3000};

    bool retry_enabled{false};
    std::size_t max_retries{0};

    bool fail_fast{true};

    std::string trace_id;
};
```

字段含义：

```text
timeout
    从调用发起到最终完成允许的最大时间。

retry_enabled
    是否允许客户端在可重试错误上重新发送请求。

max_retries
    最多重试次数，不包含第一次请求。

fail_fast
    如果当前未连接，是否立即失败。
    true  表示立即返回 NetworkError。
    false 表示允许请求等待连接恢复。

trace_id
    调用链追踪标识。为空时 RpcClient 可以自动生成。
```

第二版先实现固定语义，不需要支持复杂 per-method 配置。

---

# 6. RpcFuture 与异步调用

第二版最重要的新增能力是异步 RPC。

建议接口：

```cpp
using RpcCallback =
    std::function<void(RpcResponse)>;

class RpcFuture {
public:
    RpcResponse get();

    bool wait();

    bool waitFor(
        std::chrono::milliseconds timeout);

    bool ready() const;

    void cancel();
};
```

`RpcClient` 增加：

```cpp
class RpcClient {
public:
    RpcFuture callAsync(
        RpcRequest request,
        RpcCallOptions options = {});

    void callAsync(
        RpcRequest request,
        RpcCallback callback,
        RpcCallOptions options = {});

    RpcResponse call(
        RpcRequest request,
        std::chrono::milliseconds timeout);
};
```

同步 `call()` 不再维护独立逻辑，而是封装异步调用：

```text
call()
  │
  ▼
callAsync()
  │
  ▼
future.get()
```

这样可以保证同步调用、Future 调用、Callback 调用都走同一套
`PendingCall` 生命周期。

---

# 7. PendingCall 生命周期

第二版需要把 PendingCall 从“同步线程等待对象”升级为“调用状态对象”。

建议：

```cpp
enum class PendingState {
    WaitingToSend,
    Sent,
    Completed,
    TimedOut,
    Cancelled,
    Failed
};

struct PendingCall {
    std::uint64_t request_id{0};
    RpcRequest request;
    RpcCallOptions options;

    PendingState state{PendingState::WaitingToSend};

    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point deadline;

    RpcCallback callback;

    std::mutex mutex;
    std::condition_variable condition;
    bool completed{false};
    RpcResponse response;
};
```

所有完成路径必须收敛到一个函数：

```cpp
void completePending(
    std::uint64_t request_id,
    RpcResponse response);
```

所有失败路径也必须收敛：

```cpp
void failPending(
    std::uint64_t request_id,
    RpcErrorCode error_code,
    std::string error_message);
```

关键规则：

```text
一个 PendingCall 只能完成一次。
完成后必须从 pending_calls_ 删除。
超时后迟到响应必须丢弃。
取消后迟到响应必须丢弃。
连接断开时已发送请求必须按策略失败或重试。
Callback 不能在持有 pending_calls_ 锁时执行。
```

---

# 8. Deadline 与超时

第一版的超时发生在调用线程：

```text
condition_variable.wait_for(timeout)
```

第二版的超时应该由 RpcClient 统一管理。

流程：

```text
callAsync()
  │
  ▼
计算 deadline
  │
  ▼
注册 PendingCall
  │
  ▼
向 EventLoop 注册 timer
  │
  ▼
超时触发
  │
  ▼
failPending(Timeout)
```

如果 MiniReactor 当前没有通用 TimerQueue，第二版有两个选择：

```text
方案 A：先在 MiniReactor 补 TimerQueue / runAfter / runEvery
方案 B：MiniRPC 内部使用一个轻量 deadline 线程扫描 PendingCall
```

推荐方案 A。

原因：

```text
超时本质上是 Reactor 常见能力。
后续自动重连退避、心跳、连接空闲检测也都需要 timer。
把 timer 放进 MiniReactor 可以被其他网络组件复用。
```

第二版可以先只实现：

```cpp
TimerId EventLoop::runAfter(
    std::chrono::milliseconds delay,
    std::function<void()> callback);

void EventLoop::cancel(
    TimerId timer_id);
```

`runEvery()` 可以留到心跳阶段再补。

---

# 9. Cancellation

取消用于主动放弃一次尚未完成的 RPC。

建议：

```cpp
class RpcFuture {
public:
    void cancel();
};
```

取消流程：

```text
RpcFuture::cancel()
  │
  ▼
RpcClient::cancel(request_id)
  │
  ▼
pending_calls_.erase(request_id)
  │
  ▼
完成 future / callback
  │
  ▼
返回 Cancelled 错误
```

新增错误码：

```cpp
enum class RpcErrorCode : int {
    Cancelled = 1004
};
```

第一阶段的协议不需要新增取消帧。

第二版取消语义只保证：

```text
客户端不再等待该响应。
客户端收到迟到响应后丢弃。
服务端已经收到的请求可以继续执行。
```

不要在第二版实现跨网络取消业务执行，因为那需要服务端 handler 协作，
会明显扩大范围。

---

# 10. ClientConnectionState

第一版 `RpcClient` 已经有 connected、connecting、acceptingCalls 等状态。
第二版应该显式收敛成状态机。

建议：

```cpp
enum class ClientConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Stopping
};
```

状态流转：

```text
Disconnected
  │ connect()
  ▼
Connecting
  │ connected
  ▼
Connected
  │ connection closed
  ▼
Reconnecting
  │ reconnect success
  ▼
Connected

Connecting
  │ connect failed / timeout
  ▼
Disconnected

Reconnecting
  │ stop()
  ▼
Stopping
```

RpcClient 增加：

```cpp
using ConnectionStateCallback =
    std::function<void(ClientConnectionState)>;

void setConnectionStateCallback(
    ConnectionStateCallback callback);
```

状态回调用于测试、日志、指标，也用于业务方观察连接状态。

---

# 11. AutoReconnect

第二版应支持自动重连，但不要默认重试已经发送出去的请求。

建议配置：

```cpp
struct ReconnectOptions {
    bool enabled{true};

    std::chrono::milliseconds initial_delay{100};
    std::chrono::milliseconds max_delay{3000};
    double backoff_multiplier{2.0};
};
```

重连流程：

```text
连接断开
  │
  ▼
标记 state = Reconnecting
  │
  ▼
fail 或保留 PendingCall
  │
  ▼
runAfter(backoff_delay)
  │
  ▼
TcpClient::connect()
  │
  ▼
连接成功后重置 backoff
```

PendingCall 处理策略：

```text
已发送请求
    默认返回 NetworkError。

等待发送请求，且 fail_fast = false
    可以保留，等待重连成功后发送。

等待发送请求，且 fail_fast = true
    立即返回 NetworkError。
```

第二版不要自动重试所有已发送请求。

原因：

```text
服务端可能已经执行了业务逻辑。
客户端不知道请求是在发送前失败、发送中失败，还是响应返回前失败。
默认自动重试会破坏非幂等方法。
```

---

# 12. RetryPolicy

第二版可以实现有限重试，但必须只针对明确安全的场景。

建议：

```cpp
enum class RetryCondition {
    ConnectFailed,
    SendFailedBeforeWrite,
    TimeoutBeforeSend
};

struct RetryPolicy {
    bool enabled{false};
    std::size_t max_retries{0};
    std::chrono::milliseconds initial_delay{50};
    std::chrono::milliseconds max_delay{1000};
};
```

允许重试：

```text
请求尚未写入连接。
连接尚未建立，且 fail_fast = false。
调用方显式开启 retry_enabled。
```

默认不重试：

```text
请求已经 send() 到 TcpConnection。
请求已经被服务端接收的可能性无法排除。
业务返回 ServerError。
ServiceNotFound / MethodNotFound。
ProtocolError。
Cancelled。
```

这个边界必须写进测试，否则重试很容易变成隐藏的重复调用。

---

# 13. RpcChannel 扩展

`RpcChannel` 在第一版只是同步封装。

第二版应该增加异步方法：

```cpp
class RpcChannel {
public:
    bool callMethod(
        std::string_view service,
        std::string_view method,
        std::string request_payload,
        std::string& response_payload,
        RpcController& controller);

    RpcFuture callMethodAsync(
        std::string_view service,
        std::string_view method,
        std::string request_payload,
        RpcCallOptions options = {});

    void callMethodAsync(
        std::string_view service,
        std::string_view method,
        std::string request_payload,
        RpcCallback callback,
        RpcCallOptions options = {});
};
```

同步 `callMethod()` 同样应该基于异步 API 实现。

`RpcController` 可以继续表示同步调用状态；异步调用优先通过
`RpcResponse::error_code` 表达结果。

---

# 14. Serializer

第一版直接使用：

```text
std::string payload
```

第二版可以引入可插拔序列化层，但不要立刻强依赖 Protobuf 编译链。

建议先定义：

```cpp
class RpcSerializer {
public:
    virtual ~RpcSerializer() = default;

    virtual std::string name() const = 0;

    virtual std::string serialize(
        const std::string& payload) = 0;

    virtual bool deserialize(
        const std::string& bytes,
        std::string& payload,
        std::string& error_message) = 0;
};
```

第一版已有的 `std::string payload` 可以对应：

```text
RawStringSerializer
```

后续再扩展：

```text
ProtobufSerializer
JsonSerializer
```

第二版可以先实现 RawStringSerializer，并在 Metadata 中增加：

```text
serializer = raw
```

这样协议仍然兼容第一版思想，但为后续 Protobuf 留好扩展点。

---

# 15. Protocol Metadata 扩展

第二版不建议修改 24 字节固定头。

原因：

```text
第一版 Codec 已经稳定处理固定头、半包、粘包和长度限制。
第二版需要扩展的是 Metadata，不是消息边界。
```

Request Metadata 可以扩展为：

```text
service_name
method_name
timeout_ms
trace_id
serializer
attempt
```

Response Metadata 可以扩展为：

```text
error_code
error_message
trace_id
server_cost_us
serializer
```

如果当前 Metadata 是自定义二进制格式，第二版可以继续使用 length-field
字符串字段，不必改成 JSON。

要求：

```text
新增字段必须有默认值。
老字段解析逻辑不能被破坏。
字段长度仍然计入 kMaxMessageSize。
未知字段可以忽略。
```

---

# 16. RpcMetrics

第二版应加入基础指标，但不要引入复杂监控系统。

建议定义：

```cpp
struct RpcCallMetrics {
    std::uint64_t total_calls{0};
    std::uint64_t success_calls{0};
    std::uint64_t failed_calls{0};
    std::uint64_t timeout_calls{0};
    std::uint64_t cancelled_calls{0};
    std::uint64_t reconnect_count{0};
};
```

`RpcClient` 统计：

```text
发起调用数
成功响应数
超时数
取消数
网络失败数
协议失败数
当前 pending 数
重连次数
```

`RpcServer` 统计：

```text
收到请求数
成功响应数
ServiceNotFound 数
MethodNotFound 数
ServerError 数
业务队列拒绝数
当前业务队列长度
```

建议接口：

```cpp
RpcCallMetrics metrics() const;
```

指标先用于测试和日志，不需要暴露 HTTP endpoint。

---

# 17. RpcTraceContext

第二版引入轻量 trace，不引入完整 OpenTelemetry。

建议：

```cpp
struct RpcTraceContext {
    std::string trace_id;
    std::uint64_t request_id{0};
    std::string service_name;
    std::string method_name;
};
```

客户端：

```text
如果 RpcCallOptions.trace_id 为空，则生成 trace_id。
trace_id 写入 request metadata。
日志中打印 trace_id + request_id。
```

服务端：

```text
从 request metadata 读取 trace_id。
响应 metadata 原样带回 trace_id。
业务执行日志中打印 trace_id + service + method。
```

这样第二版可以做到：

```text
一次 RPC 从客户端发起、服务端处理、客户端收到响应，日志上可以串起来。
```

---

# 18. 错误码扩展

第一版已有：

```text
Ok
NetworkError
ProtocolError
Timeout
ServiceNotFound
MethodNotFound
ServerError
```

第二版建议增加：

```cpp
enum class RpcErrorCode : int {
    Cancelled = 1004,
    ClientStopping = 1005,
    QueueingTimeout = 1006,
    RetryExhausted = 1007,
    SerializationError = 4001,
    DeserializationError = 4002
};
```

语义：

```text
Cancelled
    调用方主动取消。

ClientStopping
    RpcClient 正在析构或 stop，不再接受请求。

QueueingTimeout
    请求允许等待连接恢复，但在发送前 deadline 已过。

RetryExhausted
    开启重试后，所有允许的重试机会用完。

SerializationError
    请求 payload 编码失败。

DeserializationError
    响应 payload 解码失败，或服务端解析请求 payload 失败。
```

错误码要保持稳定，不要在测试中依赖错误字符串。

---

# 19. RpcServer 第二版调整

第二版的服务端不需要大改，但需要配合以下能力：

```text
读取 trace_id / timeout_ms / serializer metadata
记录请求处理耗时
在响应中带回 trace_id / server_cost_us
在业务队列满时返回明确错误
捕获序列化和反序列化错误
```

服务端处理流程：

```text
TcpConnection 收到数据
        │
        ▼
Codec 解码 RpcRequest
        │
        ▼
解析 RpcTraceContext
        │
        ▼
检查 deadline 是否已经过期
        │
        ├── 是 → 返回 Timeout
        │
        ▼
ServiceRegistry 查找方法
        │
        ▼
提交业务线程池
        │
        ▼
执行 MethodHandler
        │
        ▼
记录 server_cost_us
        │
        ▼
发送 RpcResponse
```

如果请求到达服务端时已经超过客户端声明的 deadline，服务端可以直接返回
`Timeout`，避免无意义执行。

---

# 20. 完整异步调用流程

假设客户端异步调用：

```text
CalculatorService.Add(10, 20)
```

流程：

```text
Client Business Code
        │
        ▼
RpcChannel::callMethodAsync()
        │
        ▼
RpcClient::callAsync()
        │
        ▼
生成 request_id + trace_id + deadline
        │
        ▼
创建 PendingCall
        │
        ▼
注册 deadline timer
        │
        ▼
连接可用？
        │
        ├── 否，fail_fast = true
        │      ↓
        │   NetworkError
        │
        ├── 否，fail_fast = false
        │      ↓
        │   等待重连
        │
        ▼
Codec Encode
        │
        ▼
TcpConnection::send()
        │
        ▼
服务端处理
        │
        ▼
客户端收到 RpcResponse
        │
        ▼
根据 request_id 找到 PendingCall
        │
        ▼
取消 deadline timer
        │
        ▼
完成 Future 或 Callback
```

超时路径：

```text
deadline timer 触发
        │
        ▼
pending_calls_.erase(request_id)
        │
        ▼
返回 Timeout
        │
        ▼
迟到响应到达时被丢弃
```

---

# 21. 线程模型

第二版线程模型建议：

```text
Main Reactor Thread
        │
        └── accept

IO Reactor Threads
        │
        ├── read RPC
        ├── decode / encode
        ├── send response
        └── run timer callbacks

Business ThreadPool
        │
        └── execute MethodHandler

Client Caller Thread
        │
        ├── call()
        ├── callAsync()
        └── future.get()

Callback Execution
        │
        └── 默认在 RpcClient 内部回调分发点执行
```

关键原则：

```text
EventLoop Thread 不能阻塞等待 RPC 结果。
Callback 不能在持有 RpcClient 内部锁时执行。
Callback 执行位置必须文档化。
PendingCall 状态变更必须线程安全。
Timer 回调与网络响应可能竞争，同一请求只能完成一次。
```

如果 Callback 默认在 EventLoop 线程执行，文档必须明确：

```text
Callback 内不能做耗时阻塞操作。
```

如果 Callback 投递到独立线程池执行，则需要额外管理 callback 线程池。
第二版推荐先采用 EventLoop 投递回调，并在文档和测试中约束回调行为。

---

# 22. 实现顺序

推荐严格按照：

```text
1. MiniReactor TimerQueue / runAfter / cancel
      ↓
2. RpcCallOptions
      ↓
3. PendingCall 生命周期重构
      ↓
4. RpcFuture
      ↓
5. RpcClient::callAsync(Future)
      ↓
6. 同步 RpcClient::call() 改为封装 callAsync()
      ↓
7. Callback 风格 callAsync()
      ↓
8. Deadline / Timeout 统一处理
      ↓
9. Cancellation
      ↓
10. ClientConnectionState
      ↓
11. AutoReconnect
      ↓
12. 有限 RetryPolicy
      ↓
13. RpcChannel 异步接口
      ↓
14. Serializer / RawStringSerializer
      ↓
15. Protocol Metadata 扩展
      ↓
16. RpcMetrics
      ↓
17. RpcTraceContext
      ↓
18. async_calculator_client 示例
      ↓
19. 端到端测试与压力测试
```

最关键的路径是：

```text
TimerQueue
    ↓
PendingCall
    ↓
callAsync()
    ↓
Deadline
    ↓
Reconnect
```

这条路径稳定后，再补序列化和指标。

---

# 23. 第二版暂不实现

以下内容继续留到第三版以后：

```text
注册中心
服务发现
多节点负载均衡
连接池
熔断
限流
流式 RPC
TLS
认证
HTTP/2
多协议支持
完整 Protobuf Stub 自动生成
跨网络取消服务端业务执行
服务端主动推送
协程 RPC
OpenTelemetry 完整接入
HTTP Metrics Endpoint
```

第二版不要因为服务治理功能把客户端调用模型复杂化。

---

# 24. 必须测试的场景

## Future 异步调用

```text
callAsync() 返回 RpcFuture。
调用线程不阻塞。
future.get() 能取得正确响应。
```

## Callback 异步调用

```text
callAsync(request, callback) 能执行 callback。
callback 收到正确 RpcResponse。
callback 只执行一次。
```

## 同步调用兼容

```text
原有 RpcClient::call() 行为保持兼容。
内部改为基于 callAsync()。
```

## Deadline 超时

```text
服务端业务故意 sleep 超过 timeout。
客户端返回 Timeout。
迟到响应被丢弃。
pending_calls_ 最终为空。
```

## Cancel

```text
调用 cancel() 后返回 Cancelled。
服务端迟到响应不会重新完成该 Future。
callback 不会重复执行。
```

## 连接断开

```text
已发送请求在连接断开后返回 NetworkError。
等待发送请求按照 fail_fast 策略处理。
```

## 自动重连

```text
服务端停止后客户端进入 Reconnecting。
服务端恢复后客户端重新 Connected。
新请求可以成功发送。
```

## 重连期间等待发送

```text
fail_fast = false 的请求可等待连接恢复。
deadline 到期前重连成功则发送。
deadline 到期仍未连接则返回 QueueingTimeout 或 Timeout。
```

## 默认不重试已发送请求

```text
请求已经 send() 后连接断开。
即使 retry_enabled = true，也不能默认重发该请求。
```

## 有限安全重试

```text
连接建立前失败。
retry_enabled = true。
max_retries 范围内可以重新连接并发送。
超过次数返回 RetryExhausted。
```

## 序列化错误

```text
Serializer 编码失败返回 SerializationError。
Deserializer 解码失败返回 DeserializationError。
```

## Metrics

```text
成功、失败、超时、取消、重连计数准确。
pending 数在调用完成后归零。
```

## Trace

```text
客户端生成 trace_id。
服务端响应带回同一个 trace_id。
日志中能看到 request_id + trace_id + service + method。
```

---

# 25. 第二版完成标准

* [ ] MiniReactor 支持 `runAfter()` 和 `cancel()`。
* [ ] RpcCallOptions 可以描述 timeout、retry、fail_fast 和 trace_id。
* [ ] PendingCall 生命周期统一，不再只服务同步等待。
* [ ] RpcFuture 支持 `get()`、`waitFor()`、`ready()` 和 `cancel()`。
* [ ] RpcClient 支持 Future 异步调用。
* [ ] RpcClient 支持 Callback 异步调用。
* [ ] 同步 `call()` 基于异步调用实现并保持兼容。
* [ ] Deadline 由 RpcClient 统一触发。
* [ ] 超时响应只完成一次，迟到响应被丢弃。
* [ ] Cancellation 可以主动取消未完成调用。
* [ ] ClientConnectionState 明确表达连接状态。
* [ ] 自动重连支持指数退避。
* [ ] 重连期间请求按照 fail_fast 策略处理。
* [ ] RetryPolicy 只重试明确安全的场景。
* [ ] RpcChannel 支持异步接口。
* [ ] Serializer 接口和 RawStringSerializer 完成。
* [ ] Metadata 扩展支持 trace_id、timeout_ms、serializer、attempt。
* [ ] RpcMetrics 可以统计客户端和服务端基础指标。
* [ ] RpcTraceContext 能贯穿请求、响应和日志。
* [ ] async_calculator_client 示例正常工作。
* [ ] 通过异步、超时、取消、断连、重连、重试测试。
* [ ] ASan 未发现明显内存问题。
* [ ] TSan 未发现明显数据竞争。

---

# 26. 第二版最值得掌握的知识

完成第二版后，重点不是功能数量，而是理解：

```text
RPC 调用生命周期 > 一次网络请求
```

第一版的核心认识是：

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

第二版应该继续补上：

```text
RPC
=
Call Lifecycle
+
Deadline
+
Cancellation
+
Connection State
+
Reconnect
+
Observability
```

其中：

```text
RpcFuture / RpcCallback
```

负责非阻塞调用；

```text
PendingCall
```

负责统一请求状态；

```text
Deadline / Timer
```

负责调用生命周期边界；

```text
ClientConnectionState / AutoReconnect
```

负责长期运行客户端；

```text
Serializer
```

负责业务对象和字节 payload 的边界；

```text
Metrics / Trace
```

负责问题定位和行为验证。

---

# 27. 第二版最终目标

第二版完成后，MiniRPC 不要求达到生产级 RPC 框架水平。

只要求真正实现：

```text
业务代码
   ↓
发起同步或异步 RPC
   ↓
RpcClient 创建 PendingCall
   ↓
Deadline 约束调用生命周期
   ↓
连接可用则发送
   ↓
连接不可用则按策略失败或等待重连
   ↓
服务端执行业务并返回响应
   ↓
客户端按 request_id 完成 Future / Callback
   ↓
超时、取消、断连、重连都有明确结果
```

只要这条链路稳定工作，第二版 MiniRPC 就算完成。

下一版再重点考虑：

```text
服务发现
负载均衡
连接池
熔断限流
Protobuf Stub 自动生成
流式 RPC
TLS / 认证
```

而不是在第二版把所有 RPC 高级功能一次性塞进去。
