# MiniReactor Client 与 MiniRPC Phase 1 设计

日期：2026-08-14

## 1. 目标

本次工作分为两个连续里程碑：

1. 为现有 MiniReactor 补齐通用的异步 TCP 客户端能力，包括
   `InetAddress`、`Connector` 和 `TcpClient`。
2. 按 `code/MiniRPC/docs/phase1.md` 实现最小可运行的 MiniRPC 调用闭环，
   客户端与服务端都建立在 MiniReactor 之上。

最终需要稳定跑通：

```text
RpcChannel
  -> RpcClient
  -> MiniReactor TcpClient
  -> TCP
  -> MiniReactor TcpServer
  -> RpcServer
  -> ServiceRegistry
  -> ThreadPool
  -> MethodHandler
  -> RpcResponse
  -> PendingCall
  -> 调用线程
```

第一版不实现 Protobuf、服务发现、连接池、自动重连、自动重试、熔断、
限流、流式 RPC、TLS、Tracing、Metrics、认证或异步 Future API。

## 2. 现状与约束

### 2.1 可复用能力

- MiniReactor 已提供 Linux `epoll` Reactor、`EventLoop`、`Channel`、
  `TcpServer`、`TcpConnection`、跨线程 `runInLoop/queueInLoop` 和线程安全
  `TcpConnection::send()`。
- MiniReactor 已经接入 AsyncLogger、TimerScheduler 和 ThreadPool。
- `code/ThreadPool` 提供工作窃取、有界全局任务队列和非阻塞
  `try_post()`。
- `code/BlockingQueue` 提供有界阻塞队列，但依赖 C++20，并且其阻塞
  `push()` 会阻塞 Reactor IO 线程。

### 2.2 当前缺口

- MiniReactor 没有客户端侧的非阻塞连接状态机。
- `Channel` 没有 `tie()`，所以连接和 Channel 必须严格在所属 loop 线程
  中移除，并推迟到当前事件回调结束后再销毁。
- `TcpConnection` 负责已建立连接，不负责 `connect()`。
- `TcpServer` 没有通用 stop 接口；第一版测试必须先结束请求，再退出 loop。
- 当前 WSL 的 CMake 是 3.16.3，而 MiniReactor 声明最低 3.20。实现验证
  使用临时目录中的 CMake 3.20 以上版本，不为了本功能降低现有构建要求。

## 3. 总体选择

采用分层方案：

```text
InetAddress  只负责 IPv4 地址值与 sockaddr_in 转换
     |
Connector    只负责一次非阻塞连接尝试
     |
TcpClient    管理 Connector 与唯一 TcpConnection
     |
RpcClient    管理协议、PendingCall 与同步等待
```

不把连接状态机直接塞入 `TcpClient`，也不让 MiniRPC 直接操作 `Channel`。
这样连接建立、连接生命周期和 RPC 请求关联可以独立测试，第二版加入重连时
也只需扩展 Connector/TcpClient，不需要改动 RPC 协议层。

## 4. MiniReactor Client 设计

### 4.1 InetAddress

新增只支持 IPv4 的轻量值对象：

```cpp
class InetAddress {
public:
    InetAddress(std::string ip, std::uint16_t port);

    const sockaddr_in& sockaddr() const noexcept;
    std::string ip() const;
    std::uint16_t port() const noexcept;
    std::string toIpPort() const;
};
```

构造时通过 `inet_pton` 验证地址；非法地址抛 `std::invalid_argument`。
第一版不加入 DNS、IPv6 和 hostname 解析。

### 4.2 Connector

Connector 是 `std::enable_shared_from_this<Connector>`，只在指定
`EventLoop` 中读写内部状态：

```cpp
enum class State { Disconnected, Connecting, Connected };

class Connector {
public:
    using NewConnectionCallback = std::function<void(int socket_fd)>;
    using ErrorCallback = std::function<void(int error)>;

    Connector(EventLoop* loop, InetAddress server_address);
    void setNewConnectionCallback(NewConnectionCallback callback);
    void setErrorCallback(ErrorCallback callback);
    void start();
    void stop();
};
```

连接流程：

```text
start()
  -> queue/run startInLoop()
  -> socket(AF_INET, SOCK_NONBLOCK | SOCK_CLOEXEC)
  -> connect()
       0             -> 立即成功
       EINPROGRESS   -> Channel 关注 EPOLLOUT
       其他错误      -> 关闭 fd，ErrorCallback(errno)

EPOLLOUT
  -> getsockopt(SO_ERROR)
       0             -> 移除 Connector Channel，转交 fd 所有权
       非 0          -> 移除 Channel，关闭 fd，报告错误
```

Connector 在连接完成前拥有原始 socket fd；成功回调开始时所有权转交给
TcpClient，失败和 stop 路径负责关闭 fd。

由于 `Channel::handleEvent()` 正在使用自身对象，Connector 在回调中只能先
`disableAll()` 和 `remove()`，随后通过 `queueInLoop()` 延迟销毁 Channel，
避免在成员函数尚未返回时释放当前对象。

第一版一次 Connector 只执行一次连接尝试，不自动重连、不指数退避。

### 4.3 TcpClient

TcpClient 组合 Connector，并将成功连接包装为现有 TcpConnection：

```cpp
class TcpClient : private NonCopyable {
public:
    using ConnectionErrorCallback = std::function<void(int error)>;

    TcpClient(EventLoop* loop, const InetAddress& server_address,
              std::string name);
    ~TcpClient();

    void setConnectionCallback(TcpConnection::ConnectionCallback callback);
    void setMessageCallback(TcpConnection::MessageCallback callback);
    void setConnectionErrorCallback(ConnectionErrorCallback callback);

    void connect();
    void disconnect();
    void stop();
    std::shared_ptr<TcpConnection> connection() const;
};
```

语义如下：

- `connect()`、`disconnect()`、`stop()` 可从其他线程调用，实际状态变化
  marshal 到 loop 线程。
- `connect()` 是异步操作；成功或失败分别通过回调报告。
- `disconnect()` 对已建立连接执行 graceful shutdown。
- `stop()` 取消连接中的 Connector，并强制关闭已建立连接。
- `connection()` 用互斥锁返回连接快照，供其他线程安全取得 shared_ptr。
- 同一个 TcpClient 第一版只管理一个连接，不承诺断开后再次 connect。

TcpClient 构造和析构必须发生在所属 loop 线程，且 EventLoop 必须比它活得
更久。这个约束避免 Connector/TcpConnection 回调捕获悬垂 `this`。
RpcClient 会封装这条约束：在内部 loop 线程创建、停止并销毁 TcpClient，
然后才结束 EventLoopThread。

TcpClient 析构时先在 loop 线程清空 Connector 和 TcpConnection 中所有指向
TcpClient 的回调，再取消 Connector、关闭并从 Poller 移除 TcpConnection，
最后释放成员。Connector 自身由 shared_ptr 保活；若当前正在处理 EPOLLOUT，
其 Channel 的最终释放仍通过下一轮 `queueInLoop()` 完成。

### 4.4 MiniReactor Client 测试

新增真实回环网络测试，不 mock epoll/socket：

- InetAddress 正确保存 IP/端口并拒绝非法 IPv4。
- TcpClient 异步连接本地 TcpServer，完成双向大消息收发。
- 连接未监听端口时触发 error callback，且不产生假连接。
- 主动 disconnect 后双方只收到一次断开事件。
- connect 后立即 stop 能确定性取消尚未交付的 Connector，不产生迟到连接。
- TcpClient 从非 loop 线程调用 connect/send/disconnect 时保持线程安全。

## 5. MiniRPC 协议与 Codec

### 5.1 固定报头

报头固定 24 字节，所有整数使用网络字节序，逐字段编码，禁止直接
`reinterpret_cast` C++ 结构体，避免填充、对齐和主机字节序问题。

```text
Magic       uint32   0x4D525043
Version     uint16   1
Type        uint16   Request=1, Response=2
RequestId   uint64
MetaLen     uint32
BodyLen     uint32
```

`metadata_length + body_length` 不得超过 16 MiB，计算总长度前必须先做
无符号整数溢出检查。

### 5.2 消息体

```cpp
struct RpcRequest {
    std::uint64_t request_id{0};
    std::string service_name;
    std::string method_name;
    std::string payload;
};

struct RpcResponse {
    std::uint64_t request_id{0};
    int error_code{0};
    std::string error_message;
    std::string payload;
};
```

Request Metadata：

```text
service_name_length uint32
service_name        bytes
method_name_length  uint32
method_name         bytes
```

Response Metadata：

```text
error_code          int32
error_message_len   uint32
error_message       bytes
```

payload 是二进制安全字符串，不解释业务格式。

### 5.3 解码结果

文档草稿中的 `bool` 无法区分半包和非法协议，因此使用：

```cpp
enum class DecodeStatus {
    Complete,
    NeedMoreData,
    ProtocolError
};
```

Codec 只在完整报文通过全部校验后消费 Buffer。调用者循环解码直到返回
`NeedMoreData`，从而同时支持半包和粘包。非法 magic、version、type、长度
或 metadata 布局返回 `ProtocolError`。

服务端遇到非法请求帧时不信任其中的 request_id，不构造响应，直接关闭该
连接；客户端遇到非法响应帧时以 ProtocolError 完成全部 PendingCall 后关闭
连接。合法帧中的业务级错误始终通过 RpcResponse 返回。

## 6. ServiceRegistry 与服务端

### 6.1 ServiceRegistry

Registry 使用两级 map 保存 `service -> method -> handler`。为了能准确区分
`ServiceNotFound` 和 `MethodNotFound`，查询返回明确状态和 handler 副本，
不返回可能因 map 扩容而失效的内部指针。

注册使用独占锁，查询使用共享锁，因此注册和并发请求不存在数据竞争；
重复的 service/method 注册返回 false。

### 6.2 RpcServer

RpcServer 组合：

- MiniReactor `TcpServer` 负责 accept、读写和连接生命周期；
- `RpcCodec` 负责帧边界与反序列化；
- `ServiceRegistry` 负责路由；
- 独立 `ThreadPool` 负责业务方法。

IO 回调中的处理顺序：

```text
Buffer
  -> 循环 decode request
  -> 查找 service/method
  -> ThreadPool::try_post(task)
       accepted    -> worker 执行业务
       queue_full  -> 立即发送 ServerError(server busy)
  -> worker 捕获业务异常
  -> encode response
  -> TcpConnection::send()
```

不直接使用 BlockingQueue：它的阻塞 push 会卡住 IO EventLoop，且把整个
MiniRPC 提升到 C++20。ThreadPool 已经内置有界队列，并提供 `try_post()`，
正好满足“IO 线程不阻塞、队列满时显式失败”的要求。

业务任务只捕获 `weak_ptr<TcpConnection>`。发送前重新 lock；连接已经释放时
直接丢弃响应。若连接对象仍存活但已经断开，现有线程安全 `send()` 会在所属
loop 中检查状态并丢弃数据，不会从工作线程直接触碰 socket。

Handler 抛出的所有 `std::exception` 和未知异常都转换为 `ServerError`，
不允许异常逃出 ThreadPool worker。

## 7. RpcClient、PendingCall 与 RpcChannel

### 7.1 RpcClient 线程模型

RpcClient 内部拥有一个 EventLoopThread，并在该线程创建通用 TcpClient：

```text
业务线程                   RpcClient Reactor 线程
   |                                |
connect() --等待 CV----------> TcpClient::connect()
   |                         connection callback
   |<-----------唤醒---------------|
   |
call() 创建 PendingCall
   |------------send----------> TcpConnection
   | 等待 CV                       |
   |                         onMessage/decode
   |<------按 request_id 唤醒-------|
```

`RpcClient::connect()` 对业务调用方表现为有超时的同步连接，但底层连接仍是
Connector 驱动的非阻塞 Reactor 操作。连接失败或超时返回 false。

`RpcClient::call()` 可以由多个业务线程并发调用，但禁止从内部 Reactor 线程
调用，否则同步等待会让该 loop 无法读取响应；这种程序员错误抛
`std::logic_error`。

### 7.2 PendingCall

```cpp
struct PendingCall {
    std::mutex mutex;
    std::condition_variable cv;
    bool completed{false};
    RpcResponse response;
};
```

RpcClient 使用原子递增的非零 request_id，并以互斥锁保护：

```cpp
std::unordered_map<std::uint64_t, std::shared_ptr<PendingCall>> pending_calls_;
```

响应到达时先从 map 中移除对应项，再填写 response、置 completed 并通知。
超时时仅移除仍指向同一个 PendingCall 的项；之后到达的迟响应被安全忽略。
连接断开、协议错误和 RpcClient 析构都会批量取出全部 PendingCall，分别以
NetworkError 或 ProtocolError 完成并唤醒，禁止调用线程永久等待。

### 7.3 RpcController 与 RpcChannel

RpcController 保存错误码、错误文本和单次调用 timeout，默认 3000ms。
RpcChannel 构造 RpcRequest，调用 RpcClient，成功时返回 payload；任何非零
响应错误或本地错误都写入 controller 并返回 false。

错误码遵循 phase1 文档：

```text
Ok                 0
NetworkError       1001
ProtocolError      1002
Timeout            1003
ServiceNotFound    2001
MethodNotFound     2002
ServerError        3001
```

## 8. 生命周期与竞态处理

### 8.1 正常销毁顺序

RpcClient 析构顺序固定为：

1. 拒绝新 call，并把全部 PendingCall 完成为 NetworkError；
2. 在内部 loop 线程执行 TcpClient::stop()；
3. 在 loop 线程销毁 TcpClient；
4. 最后退出并 join EventLoopThread。

RpcClient 不允许从自己的内部 loop 线程析构，避免线程自 join。

### 8.2 关键竞态

- 响应与超时同时发生：pending map 的互斥锁决定唯一完成者。
- connect 成功与 connect 超时同时发生：连接状态互斥锁决定返回值，超时方
  请求 stop；迟到成功不会恢复已失败的连接状态。
- 服务端 Handler 完成与客户端断开同时发生：weak_ptr 和
  TcpConnection::send() 的 loop 内状态检查共同保证安全。
- Connector stop 与 EPOLLOUT 同时发生：两者都在同一 EventLoop 串行执行，
  只有状态为 Connecting 的路径能取得 fd 所有权。
- TcpClient 销毁与连接回调：销毁本身在所属 loop 线程执行，并且先 stop、
  清理 Connector Channel 和 TcpConnection，避免悬垂回调。

## 9. 文件布局

MiniReactor 新增：

```text
code/MiniReactor/include/net/InetAddress.h
code/MiniReactor/include/net/Connector.h
code/MiniReactor/include/net/TcpClient.h
code/MiniReactor/src/net/InetAddress.cpp
code/MiniReactor/src/net/Connector.cpp
code/MiniReactor/src/net/TcpClient.cpp
code/MiniReactor/test/TcpClientTest.cpp
```

MiniRPC 新增：

```text
code/MiniRPC/CMakeLists.txt
code/MiniRPC/include/mini_rpc/protocol.h
code/MiniRPC/include/mini_rpc/codec.h
code/MiniRPC/include/mini_rpc/service_registry.h
code/MiniRPC/include/mini_rpc/rpc_server.h
code/MiniRPC/include/mini_rpc/rpc_client.h
code/MiniRPC/include/mini_rpc/rpc_controller.h
code/MiniRPC/include/mini_rpc/rpc_channel.h
code/MiniRPC/src/*.cpp
code/MiniRPC/examples/calculator_server.cpp
code/MiniRPC/examples/calculator_client.cpp
code/MiniRPC/tests/codec_test.cpp
code/MiniRPC/tests/registry_test.cpp
code/MiniRPC/tests/rpc_test.cpp
```

MiniRPC 通过 `add_subdirectory(../MiniReactor ...)` 复用 minireactor target；
服务端直接复用 `ThreadPool.cpp` 已经进入 minireactor target 的事实，不重复
编译第二份 ThreadPool 对象。

## 10. TDD 实施顺序

每一步先写可观察行为测试并确认因缺失功能而失败，再写最小实现：

1. InetAddress 行为测试与实现。
2. Connector/TcpClient 连接、拒绝、收发和断开测试与实现。
3. Protocol/Codec request round-trip、response round-trip 测试与实现。
4. Codec 半包、粘包、非法 magic/version/type/length 和边界测试。
5. ServiceRegistry 注册、重复注册和两类未找到测试与实现。
6. RpcServer 正常调用、业务异常和线程池投递测试与实现。
7. RpcClient 单请求、timeout、断连唤醒测试与实现。
8. 同连接多线程并发、响应乱序匹配测试与实现。
9. RpcController/RpcChannel 行为测试与实现。
10. Calculator 示例和完整回归验证。

## 11. 验证矩阵

功能测试必须覆盖 phase1 文档列出的场景：

- CalculatorService.Add 正常返回；
- ServiceNotFound；
- MethodNotFound；
- request 与 response 半包；
- 连续多帧粘包；
- 同一 RpcClient 多线程并发请求及乱序响应；
- 接近 16 MiB 的合法消息与超过限制的非法消息；
- 非法 magic、version 和 message type；
- 客户端提前断开时服务端业务完成不崩溃；
- 连接失败、调用超时和连接中断唤醒等待者。

完成前执行：

```bash
cmake -S code/MiniRPC -B <debug-build> -DCMAKE_BUILD_TYPE=Debug
cmake --build <debug-build> -j2
ctest --test-dir <debug-build> --output-on-failure

cmake -S code/MiniRPC -B <asan-build> \
  -DCMAKE_BUILD_TYPE=Debug -DMINIRPC_ENABLE_ASAN=ON
cmake --build <asan-build> -j2
ctest --test-dir <asan-build> --output-on-failure

cmake -S code/MiniRPC -B <tsan-build> \
  -DCMAKE_BUILD_TYPE=Debug -DMINIRPC_ENABLE_TSAN=ON
cmake --build <tsan-build> -j2
ctest --test-dir <tsan-build> --output-on-failure
```

ASan 和 TSan 分开构建。若环境或 sanitizer 本身限制测试运行，必须报告具体
命令、退出码和错误，不能把普通 Debug 测试通过等同于 sanitizer 通过。

## 12. 完成标准

- 通用 TcpClient 完全通过 Reactor 建连、收发和断开，不创建专用收包线程。
- MiniRPC 客户端和服务端都复用 MiniReactor。
- 业务 Handler 不在 IO EventLoop 中执行。
- Codec 对半包、粘包、非法报文和长度上限行为明确。
- request_id 能在并发和乱序响应下正确匹配 PendingCall。
- 所有本地错误、远端错误、超时和断连都会结束等待，不永久阻塞。
- Calculator 示例可独立运行。
- Debug、ASan、TSan 的实际验证结果分别记录，现有基线问题与新增问题分开。
