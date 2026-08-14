# MiniReactor Client and MiniRPC Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 MiniReactor 增加通用异步 TCP 客户端，并在其上实现 `code/MiniRPC/docs/phase1.md` 规定的同步 MiniRPC 第一版闭环。

**Architecture:** MiniReactor 使用 `InetAddress -> Connector -> TcpClient -> TcpConnection` 分层；MiniRPC 使用固定长度报头、三态 Codec、ServiceRegistry、独立 ThreadPool 和 `request_id -> PendingCall` 关联。RpcClient 自有 EventLoopThread，但所有网络操作仍由 Reactor 驱动。

**Tech Stack:** Linux/WSL、C++17、CMake 3.20+、GCC、epoll、POSIX sockets、MiniReactor、ThreadPool、AsyncLogger、CTest、ASan、TSan。

**Spec:** `docs/superpowers/specs/2026-08-14-minireactor-client-minirpc-phase1-design.md`

## Global Constraints

- 以 `code/MiniRPC/docs/phase1.md` 为第一版范围，不加入 Protobuf、自动重连、自动重试、连接池、Future API 或服务治理。
- MiniReactor 和 MiniRPC 保持 C++17；不直接使用要求 C++20 且会阻塞 IO 线程的 BlockingQueue。
- 网络整数全部使用网络字节序；单帧 metadata 加 body 最大 `16 * 1024 * 1024` 字节。
- Connector、TcpConnection 和 TcpClient 的 Channel 注册、移除、销毁全部发生在所属 EventLoop 线程。
- `RpcClient::call()` 不得在其内部 Reactor 线程调用。
- IO EventLoop 只解码、路由和投递，业务 Handler 在 ThreadPool worker 中执行。
- 测试使用真实回环 socket 和真实 Reactor，不 mock epoll、Channel、TcpConnection 或 ThreadPool。
- 新增代码注释和文档使用中文；命名、花括号和 CMake 风格跟随现有 MiniReactor。
- 不修改或提交用户已有的 `.vscode/settings.json`、`CLAUDE.md`、`code/MiniRPC/docs/phase1.md` 工作区状态。
- 当前 WSL CMake 3.16.3 不满足要求；执行时使用 `/tmp/codex-cmake/bin/cmake` 的临时 CMake 3.20+，不降低仓库最低版本。

---

## File Map

### MiniReactor

- `include/net/InetAddress.h`, `src/net/InetAddress.cpp`：IPv4 地址值对象。
- `include/net/Connector.h`, `src/net/Connector.cpp`：一次异步非阻塞连接尝试和 fd 所有权转移。
- `include/net/TcpClient.h`, `src/net/TcpClient.cpp`：管理 Connector 和唯一 TcpConnection。
- `test/InetAddressTest.cpp`：地址转换与非法地址测试。
- `test/ConnectorTest.cpp`：连接成功、拒绝和 stop 状态机测试。
- `test/TcpClientTest.cpp`：真实回环收发、跨线程调用和断开测试。
- `CMakeLists.txt`：把新增源文件和测试加入现有 target。

### MiniRPC

- `include/mini_rpc/protocol.h`：消息结构、错误码、常量。
- `include/mini_rpc/codec.h`, `src/codec.cpp`：帧编码、三态流式解码。
- `include/mini_rpc/service_registry.h`, `src/service_registry.cpp`：线程安全服务注册与精确查询结果。
- `include/mini_rpc/rpc_server.h`, `src/rpc_server.cpp`：TcpServer、Codec、Registry、ThreadPool 调度。
- `include/mini_rpc/rpc_client.h`, `src/rpc_client.cpp`：内部 Reactor、连接状态、PendingCall 和同步 call。
- `include/mini_rpc/rpc_controller.h`, `src/rpc_controller.cpp`：一次调用的 timeout/error 状态。
- `include/mini_rpc/rpc_channel.h`, `src/rpc_channel.cpp`：业务调用门面。
- `tests/codec_test.cpp`：帧边界、半包、粘包、非法帧和长度限制。
- `tests/registry_test.cpp`：注册、重复、ServiceNotFound、MethodNotFound。
- `tests/rpc_server_test.cpp`：使用 MiniReactor TcpClient 驱动 RpcServer。
- `tests/rpc_client_test.cpp`：同步调用、并发乱序、timeout、断连唤醒。
- `tests/rpc_channel_test.cpp`：controller/channel 映射。
- `examples/calculator_server.cpp`, `examples/calculator_client.cpp`：可运行示例。
- `CMakeLists.txt`：独立工程入口、依赖组合、测试及 sanitizer 选项。

---

### Task 1: InetAddress IPv4 值对象

**Files:**
- Create: `code/MiniReactor/include/net/InetAddress.h`
- Create: `code/MiniReactor/src/net/InetAddress.cpp`
- Create: `code/MiniReactor/test/InetAddressTest.cpp`
- Modify: `code/MiniReactor/CMakeLists.txt`

**Interfaces:**
- Consumes: POSIX `sockaddr_in`, `inet_pton`, `inet_ntop`, `htons`, `ntohs`。
- Produces: `InetAddress(std::string, uint16_t)`, `sockaddr()`, `ip()`, `port()`, `toIpPort()`。

- [ ] **Step 0: 准备临时 CMake 3.31.12 并校验官方 SHA-256**

```bash
wsl bash -lc 'set -e; if [ ! -x /tmp/codex-cmake/bin/cmake ]; then mkdir -p /tmp/codex-cmake /tmp/codex-cmake-download; cd /tmp/codex-cmake-download; curl -fL -o cmake.tar.gz https://github.com/Kitware/CMake/releases/download/v3.31.12/cmake-3.31.12-linux-x86_64.tar.gz; curl -fL -o SHA-256.txt https://github.com/Kitware/CMake/releases/download/v3.31.12/cmake-3.31.12-SHA-256.txt; grep "cmake-3.31.12-linux-x86_64.tar.gz" SHA-256.txt | sed "s#cmake-3.31.12-linux-x86_64.tar.gz#cmake.tar.gz#" | sha256sum -c -; tar -xzf cmake.tar.gz -C /tmp/codex-cmake --strip-components=1; fi; /tmp/codex-cmake/bin/cmake --version'
```

Expected: SHA-256 检查输出 `cmake.tar.gz: OK`，版本输出 `cmake version 3.31.12`。所有文件只位于 `/tmp`。

- [ ] **Step 1: 写地址行为测试并注册 CTest**

```cpp
int main() {
    minireactor::InetAddress address("127.0.0.1", 8080);
    assert(address.ip() == "127.0.0.1");
    assert(address.port() == 8080);
    assert(address.toIpPort() == "127.0.0.1:8080");
    assert(address.sockaddr().sin_family == AF_INET);
    assert(ntohs(address.sockaddr().sin_port) == 8080);

    bool rejected = false;
    try {
        minireactor::InetAddress invalid("300.1.1.1", 1);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
```

- [ ] **Step 2: 运行测试并确认 RED**

Run: `wsl bash -lc '/tmp/codex-cmake/bin/cmake -S /mnt/d/my_project/Unix/code/MiniReactor -B /tmp/minireactor-tdd && /tmp/codex-cmake/bin/cmake --build /tmp/minireactor-tdd -j2 --target inet_address_test'`

Expected: 编译失败，提示 `net/InetAddress.h` 不存在。

- [ ] **Step 3: 实现最小 InetAddress**

```cpp
namespace minireactor {
class InetAddress {
public:
    InetAddress(std::string ip, std::uint16_t port);
    const sockaddr_in& sockaddr() const noexcept;
    std::string ip() const;
    std::uint16_t port() const noexcept;
    std::string toIpPort() const;
private:
    sockaddr_in address_{};
};
}
```

构造函数将 family 设为 `AF_INET`、port 设为 `htons(port)`；`inet_pton` 返回值不是 1 时抛 `std::invalid_argument`。`ip()` 用 `inet_ntop`，失败时抛 `std::system_error`。

- [ ] **Step 4: 运行 GREEN 与 MiniReactor 回归**

Run: `wsl bash -lc '/tmp/codex-cmake/bin/cmake --build /tmp/minireactor-tdd -j2 && ctest --test-dir /tmp/minireactor-tdd --output-on-failure'`

Expected: `inet_address_test` 和现有测试全部 PASS。

- [ ] **Step 5: 提交 Task 1**

```bash
git add code/MiniReactor/CMakeLists.txt code/MiniReactor/include/net/InetAddress.h code/MiniReactor/src/net/InetAddress.cpp code/MiniReactor/test/InetAddressTest.cpp
git commit -m "feat(minireactor): add IPv4 address value type"
```

### Task 2: Connector 非阻塞连接状态机

**Files:**
- Create: `code/MiniReactor/include/net/Connector.h`
- Create: `code/MiniReactor/src/net/Connector.cpp`
- Create: `code/MiniReactor/test/ConnectorTest.cpp`
- Modify: `code/MiniReactor/CMakeLists.txt`

**Interfaces:**
- Consumes: `EventLoop::runInLoop/queueInLoop`, `Channel`, `Socket::createNonblocking`, `InetAddress::sockaddr()`。
- Produces: `Connector::start()`, `stop()`, `setNewConnectionCallback(function<void(int)>)`, `setErrorCallback(function<void(int)>)`。

- [ ] **Step 1: 写真实连接成功与拒绝测试**

测试创建一个回环监听 socket 和一个 `EventLoopThread`。在 loop 中构造 shared Connector；成功回调断言取得有效 fd 并完成 promise，错误回调记录 errno。第二段连接一个已绑定后关闭的回环端口，断言错误回调发生且成功回调没有发生。第三段在 `start()` 后立即 `stop()`，断言不会交付迟到 fd。

核心断言：

```cpp
assert(connected.wait_for(2s) == std::future_status::ready);
assert(connected.get() >= 0);
assert(refused.wait_for(2s) == std::future_status::ready);
assert(refused.get() != 0);
assert(stopped_connection.wait_for(200ms) == std::future_status::timeout);
```

- [ ] **Step 2: 运行测试并确认 RED**

Run: `wsl bash -lc '/tmp/codex-cmake/bin/cmake -S /mnt/d/my_project/Unix/code/MiniReactor -B /tmp/minireactor-tdd && /tmp/codex-cmake/bin/cmake --build /tmp/minireactor-tdd -j2 --target connector_test'`

Expected: 编译失败，提示 `net/Connector.h` 不存在。

- [ ] **Step 3: 实现 Connector 公共接口和 loop 内状态机**

```cpp
class Connector : public std::enable_shared_from_this<Connector>, private NonCopyable {
public:
    using NewConnectionCallback = std::function<void(int)>;
    using ErrorCallback = std::function<void(int)>;
    Connector(EventLoop* loop, InetAddress serverAddress);
    ~Connector();
    void setNewConnectionCallback(NewConnectionCallback callback);
    void setErrorCallback(ErrorCallback callback);
    void start();
    void stop();
private:
    enum class State { kDisconnected, kConnecting, kConnected };
    void startInLoop();
    void stopInLoop();
    void connecting(int socketFd);
    void handleWrite();
    void handleError();
    int removeAndResetChannel();
    void reportError(int socketFd, int error);
};
```

`connect()` 返回 0 时直接交付；返回 `EINPROGRESS/EINTR/EISCONN` 时进入 Connecting 并监听 EPOLLOUT；其他 errno 关闭 fd 并报告。`handleWrite()` 通过 `getsockopt(SO_ERROR)` 判断最终结果。

- [ ] **Step 4: 实现 Channel 延迟释放与 fd 单一所有权**

`removeAndResetChannel()` 必须在 loop 线程先 `disableAll()`、`remove()`，保存 fd，然后用捕获 `shared_from_this()` 的 `queueInLoop()` 在当前事件回调返回后 reset Channel。成功路径将内部 fd 标记为不再拥有；失败/stop 路径恰好关闭一次。

- [ ] **Step 5: 运行 Connector GREEN 与回归**

Run: `wsl bash -lc '/tmp/codex-cmake/bin/cmake --build /tmp/minireactor-tdd -j2 && ctest --test-dir /tmp/minireactor-tdd -R "connector_test|echo_server_test|event_loop_thread_test" --output-on-failure'`

Expected: 选中的测试全部 PASS，无重复 promise、double-close 或悬垂 Channel。

- [ ] **Step 6: 提交 Task 2**

```bash
git add code/MiniReactor/CMakeLists.txt code/MiniReactor/include/net/Connector.h code/MiniReactor/src/net/Connector.cpp code/MiniReactor/test/ConnectorTest.cpp
git commit -m "feat(minireactor): add asynchronous connector"
```

### Task 3: 通用 TcpClient

**Files:**
- Create: `code/MiniReactor/include/net/TcpClient.h`
- Create: `code/MiniReactor/src/net/TcpClient.cpp`
- Create: `code/MiniReactor/test/TcpClientTest.cpp`
- Modify: `code/MiniReactor/CMakeLists.txt`

**Interfaces:**
- Consumes: `Connector`, `TcpConnection`, `EventLoop`, `InetAddress`。
- Produces: `TcpClient::connect/disconnect/stop/connection` 和三类回调 setter。

- [ ] **Step 1: 写回环 Echo 集成测试**

服务端使用现有 `TcpServer`，客户端使用新 TcpClient。测试从主测试线程调用 `connect()`，连接回调成功后从主线程取得 `connection()` 并发送 64 KiB payload，消息回调断言完整回显；随后从主线程调用 `disconnect()`，断言客户端断开回调只发生一次。

```cpp
client->setConnectionCallback([&](const std::shared_ptr<minireactor::TcpConnection>& connection) {
    if (connection->state() == minireactor::TcpConnection::State::kConnected) {
        connected.set_value();
    } else if (disconnectCount.fetch_add(1) == 0) {
        disconnected.set_value();
    }
});
client->setMessageCallback([&](const std::shared_ptr<minireactor::TcpConnection>&, minireactor::Buffer* buffer) {
    echoed.set_value(buffer->retrieveAllAsString());
});
```

- [ ] **Step 2: 运行测试并确认 RED**

Run: `wsl bash -lc '/tmp/codex-cmake/bin/cmake --build /tmp/minireactor-tdd -j2 --target tcp_client_test'`

Expected: 编译失败，提示 `net/TcpClient.h` 不存在。

- [ ] **Step 3: 实现 TcpClient 建连和 connection 快照**

```cpp
class TcpClient : private NonCopyable {
public:
    using ConnectionErrorCallback = std::function<void(int)>;
    TcpClient(EventLoop* loop, const InetAddress& address, std::string name);
    ~TcpClient();
    void setConnectionCallback(TcpConnection::ConnectionCallback callback);
    void setMessageCallback(TcpConnection::MessageCallback callback);
    void setConnectionErrorCallback(ConnectionErrorCallback callback);
    void connect();
    void disconnect();
    void stop();
    std::shared_ptr<TcpConnection> connection() const;
private:
    void newConnection(int socketFd);
    void removeConnection(const std::shared_ptr<TcpConnection>& connection);
};
```

`newConnection()` 只能在 loop 线程执行，创建 TcpConnection、安装用户回调和 close callback、在 mutex 下发布 `connection_`，最后调用 `connectEstablished()`。

- [ ] **Step 4: 实现 stop 与析构清理**

析构断言当前是 loop 线程；先清除 Connector callback，再 stop Connector。若 connection 存在，清空其 connection/message/close callback，调用 `forceClose()` 和 `connectDestroyed()`，然后清空受 mutex 保护的 connection_。禁止让任何排队回调继续访问已析构 TcpClient。

- [ ] **Step 5: 运行 TcpClient GREEN 与 MiniReactor 全回归**

Run: `wsl bash -lc '/tmp/codex-cmake/bin/cmake --build /tmp/minireactor-tdd -j2 && ctest --test-dir /tmp/minireactor-tdd --output-on-failure'`

Expected: MiniReactor 全部测试 PASS。

- [ ] **Step 6: 提交 Task 3**

```bash
git add code/MiniReactor/CMakeLists.txt code/MiniReactor/include/net/TcpClient.h code/MiniReactor/src/net/TcpClient.cpp code/MiniReactor/test/TcpClientTest.cpp
git commit -m "feat(minireactor): add reactor tcp client"
```

### Task 4: MiniRPC 工程骨架、Protocol 与 Codec

**Files:**
- Create: `code/MiniRPC/CMakeLists.txt`
- Create: `code/MiniRPC/include/mini_rpc/protocol.h`
- Create: `code/MiniRPC/include/mini_rpc/codec.h`
- Create: `code/MiniRPC/src/codec.cpp`
- Create: `code/MiniRPC/tests/codec_test.cpp`

**Interfaces:**
- Consumes: `minireactor::Buffer`。
- Produces: `RpcRequest`, `RpcResponse`, `RpcErrorCode`, `MessageType`, `DecodeStatus`, `RpcCodec::encodeRequest/encodeResponse/tryDecodeRequest/tryDecodeResponse`。

- [ ] **Step 1: 写 Codec round-trip、半包和粘包测试**

用手工字面量构造 request/response。把编码结果逐字节追加到 Buffer，在最后一个字节前每次都断言 `NeedMoreData`；完整后断言字段相等。把三帧连接后追加到同一 Buffer，连续三次断言 Complete，最终 readableBytes 为 0。

```cpp
const minirpc::RpcRequest request{42, "CalculatorService", "Add", std::string("a\0b", 3)};
const std::string frame = codec.encodeRequest(request);
minireactor::Buffer fragmented;
for (std::size_t index = 0; index + 1 < frame.size(); ++index) {
    fragmented.append(frame.data() + index, 1);
    minirpc::RpcRequest decoded;
    assert(codec.tryDecodeRequest(fragmented, decoded) == minirpc::DecodeStatus::NeedMoreData);
}
```

- [ ] **Step 2: 写非法协议与大小边界测试**

从一条合法编码帧复制并分别破坏 magic、version、type、MetaLen、metadata 内部字符串长度；断言 ProtocolError 且 Buffer 未被误消费。构造 metadata+body 恰好 16 MiB 的合法帧应可编码，超过 1 字节时 `encodeRequest` 抛 `std::length_error`。

- [ ] **Step 3: 配置并运行 MiniRPC 测试确认 RED**

Run: `wsl bash -lc '/tmp/codex-cmake/bin/cmake -S /mnt/d/my_project/Unix/code/MiniRPC -B /tmp/minirpc-tdd -DCMAKE_BUILD_TYPE=Debug && /tmp/codex-cmake/bin/cmake --build /tmp/minirpc-tdd -j2 --target codec_test'`

Expected: 编译失败，提示 Protocol/Codec 类型尚未定义。

- [ ] **Step 4: 实现固定 24 字节网络序编码**

在 codec.cpp 内提供私有 `appendUint16/32/64` 和 `readUint16/32/64`。64 位通过两个 32 位网络序半部实现，不依赖非标准 `htonll`。编码前分别验证 metadata 内部长度可装入 uint32，且 `metadata.size() + payload.size() <= kMaxMessageSize`。

- [ ] **Step 5: 实现三态流式解码**

先检查 24 字节头；显式解析字段并验证 magic/version/type/长度与整数溢出；完整帧不足返回 NeedMoreData。完整后先在局部 string_view 中验证 metadata 布局，构造结果成功后才 `buffer.retrieve(totalLength)`。请求解码只接受 Request，响应解码只接受 Response。

- [ ] **Step 6: 运行 Codec GREEN**

Run: `wsl bash -lc '/tmp/codex-cmake/bin/cmake --build /tmp/minirpc-tdd -j2 && ctest --test-dir /tmp/minirpc-tdd -R codec_test --output-on-failure'`

Expected: `codec_test` PASS。

- [ ] **Step 7: 提交 Task 4**

```bash
git add code/MiniRPC/CMakeLists.txt code/MiniRPC/include/mini_rpc/protocol.h code/MiniRPC/include/mini_rpc/codec.h code/MiniRPC/src/codec.cpp code/MiniRPC/tests/codec_test.cpp
git commit -m "feat(minirpc): add protocol and streaming codec"
```

### Task 5: ServiceRegistry

**Files:**
- Create: `code/MiniRPC/include/mini_rpc/service_registry.h`
- Create: `code/MiniRPC/src/service_registry.cpp`
- Create: `code/MiniRPC/tests/registry_test.cpp`
- Modify: `code/MiniRPC/CMakeLists.txt`

**Interfaces:**
- Consumes: `RpcRequest`, `RpcResponse`。
- Produces: `MethodHandler`, `LookupStatus`, `LookupResult`, `registerMethod()`, `findMethod()`。

- [ ] **Step 1: 写注册与精确错误分类测试**

```cpp
minirpc::ServiceRegistry registry;
assert(registry.registerMethod("CalculatorService", "Add", handler));
assert(!registry.registerMethod("CalculatorService", "Add", handler));
assert(registry.findMethod("Missing", "Add").status == minirpc::LookupStatus::ServiceNotFound);
assert(registry.findMethod("CalculatorService", "Missing").status == minirpc::LookupStatus::MethodNotFound);
auto found = registry.findMethod("CalculatorService", "Add");
assert(found.status == minirpc::LookupStatus::Found);
assert(static_cast<bool>(found.handler));
```

再由八个线程并发执行查询和 handler 副本，断言结果计数为 8000。

- [ ] **Step 2: 运行测试并确认 RED**

Run: `wsl bash -lc '/tmp/codex-cmake/bin/cmake --build /tmp/minirpc-tdd -j2 --target registry_test'`

Expected: 编译失败，提示 `service_registry.h` 不存在。

- [ ] **Step 3: 实现 shared_mutex 保护的两级 map**

```cpp
enum class LookupStatus { Found, ServiceNotFound, MethodNotFound };
struct LookupResult {
    LookupStatus status{LookupStatus::ServiceNotFound};
    MethodHandler handler;
};
```

注册持有 `std::unique_lock<std::shared_mutex>`，重复 key 返回 false；查询持有 `std::shared_lock`，返回 handler 副本而不是内部指针。

- [ ] **Step 4: 运行 Registry GREEN**

Run: `wsl bash -lc '/tmp/codex-cmake/bin/cmake --build /tmp/minirpc-tdd -j2 && ctest --test-dir /tmp/minirpc-tdd -R registry_test --output-on-failure'`

Expected: `registry_test` PASS。

- [ ] **Step 5: 提交 Task 5**

```bash
git add code/MiniRPC/CMakeLists.txt code/MiniRPC/include/mini_rpc/service_registry.h code/MiniRPC/src/service_registry.cpp code/MiniRPC/tests/registry_test.cpp
git commit -m "feat(minirpc): add thread-safe service registry"
```

### Task 6: RpcServer 调度闭环

**Files:**
- Create: `code/MiniRPC/include/mini_rpc/rpc_server.h`
- Create: `code/MiniRPC/src/rpc_server.cpp`
- Create: `code/MiniRPC/tests/rpc_server_test.cpp`
- Modify: `code/MiniRPC/CMakeLists.txt`

**Interfaces:**
- Consumes: MiniReactor `TcpServer/TcpClient`, `RpcCodec`, `ServiceRegistry`, `ThreadPool::try_post()`。
- Produces: `RpcServer(EventLoop*, InetAddress, ioThreads, workerThreads, queueCapacity)`, `registerMethod()`, `start()`。

- [ ] **Step 1: 写由 Reactor TcpClient 驱动的正常调用测试**

启动 RpcServer，注册 `CalculatorService.Add`，Handler 解析 payload `"10 20"` 并写入 `"30"`。测试客户端使用 MiniReactor TcpClient 发送 `codec.encodeRequest()`，在消息回调中解码响应并断言 request_id=1001、error_code=0、payload=`"30"`。

- [ ] **Step 2: 扩展 ServiceNotFound、MethodNotFound 和异常测试**

依次发送 UnknownService.Add、CalculatorService.Unknown 和一个抛 `runtime_error("boom")` 的方法，断言错误码分别为 2001、2002、3001，且每条响应保留原 request_id。

- [ ] **Step 3: 运行测试并确认 RED**

Run: `wsl bash -lc '/tmp/codex-cmake/bin/cmake --build /tmp/minirpc-tdd -j2 --target rpc_server_test'`

Expected: 编译失败，提示 `rpc_server.h` 不存在。

- [ ] **Step 4: 实现 onMessage 解码循环和错误路由**

Complete 时调用 registry；NeedMoreData 时退出回调保留 Buffer；ProtocolError 时记录日志并 `connection->forceClose()`。Service/Method 未找到时直接在 IO 线程编码轻量错误响应，不进入业务池。

- [ ] **Step 5: 实现 ThreadPool 非阻塞投递**

每个业务任务捕获 request 副本和 `weak_ptr<TcpConnection>`。`try_post()` 返回 queue_full/pool_stopping 时发送 ServerError；worker 中用 try/catch 转换异常，最后 lock weak connection 并调用线程安全 send。禁止使用 `post_wait()`。

- [ ] **Step 6: 运行 RpcServer GREEN 与现有回归**

Run: `wsl bash -lc '/tmp/codex-cmake/bin/cmake --build /tmp/minirpc-tdd -j2 && ctest --test-dir /tmp/minirpc-tdd -R "rpc_server_test|codec_test|registry_test|tcp_client_test" --output-on-failure'`

Expected: 选中的测试全部 PASS。

- [ ] **Step 7: 提交 Task 6**

```bash
git add code/MiniRPC/CMakeLists.txt code/MiniRPC/include/mini_rpc/rpc_server.h code/MiniRPC/src/rpc_server.cpp code/MiniRPC/tests/rpc_server_test.cpp
git commit -m "feat(minirpc): dispatch requests through rpc server"
```

### Task 7: RpcClient、PendingCall 与同步调用

**Files:**
- Create: `code/MiniRPC/include/mini_rpc/rpc_client.h`
- Create: `code/MiniRPC/src/rpc_client.cpp`
- Create: `code/MiniRPC/tests/rpc_client_test.cpp`
- Modify: `code/MiniRPC/CMakeLists.txt`

**Interfaces:**
- Consumes: `EventLoopThread`, MiniReactor `TcpClient`, `RpcCodec`, `RpcServer`。
- Produces: `RpcClient(ip, port)`, `connect(timeout)`, `call(request, timeout)`, `connected()`。

- [ ] **Step 1: 写单请求和连接失败测试**

RpcServer 启动后构造 RpcClient，断言 `connect(2s)` 为 true，`call()` 返回正确 Add 结果。对未监听端口构造第二个 RpcClient，断言 connect 返回 false 且耗时小于 2 秒。

- [ ] **Step 2: 写并发乱序响应测试**

Handler 根据 payload 中 delay 毫秒数 sleep，再返回原序号。由同一 RpcClient 的八个调用线程发送不同 delay，最长请求最先发送；断言每个 future 得到与自身 request 对应的 payload，而不是按到达顺序错配。

- [ ] **Step 3: 写 timeout 与断连唤醒测试**

慢 Handler sleep 300ms，call timeout 50ms，断言 error_code=1003 且迟响应不污染后续调用。另一个测试在慢调用等待时强制停止服务端连接，断言等待线程在 2 秒内返回 NetworkError，而不是永久阻塞。

- [ ] **Step 4: 运行测试并确认 RED**

Run: `wsl bash -lc '/tmp/codex-cmake/bin/cmake --build /tmp/minirpc-tdd -j2 --target rpc_client_test'`

Expected: 编译失败，提示 `rpc_client.h` 不存在。

- [ ] **Step 5: 实现内部 EventLoopThread 与同步 connect**

构造时 `startLoop()`，通过 loop promise 在内部线程创建 MiniReactor TcpClient。connect 安装状态回调后发起异步连接，并在调用线程用 CV wait_for；error callback 和 timeout 都把 connecting 状态变为失败，timeout 同时请求 TcpClient::stop()。

- [ ] **Step 6: 实现 request_id 与 PendingCall 唯一完成**

```cpp
struct PendingCall {
    std::mutex mutex;
    std::condition_variable cv;
    bool completed{false};
    RpcResponse response;
};
```

call 用 atomic fetch_add 生成非零 id，在 pending mutex 下插入 shared PendingCall，取得 connection 快照并 send。响应回调在 pending map 中 erase 后完成对象。timeout 仅在 map 仍指向同一对象时 erase；唯一取得对象的一方负责写 response 和 notify。

- [ ] **Step 7: 实现协议错误、断连与析构批量完成**

客户端 Codec 返回 ProtocolError 时用 1002 完成全部 pending 并 stop 连接；connection callback 收到 disconnected 时用 1001 完成全部 pending。析构先禁止新 call，再在 loop 线程 stop 并销毁 TcpClient，最后让 EventLoopThread quit/join。内部 EventLoop 不作为公共接口暴露，因此公共 API 不提供从该线程销毁 RpcClient 的入口。

- [ ] **Step 8: 运行 RpcClient GREEN**

Run: `wsl bash -lc '/tmp/codex-cmake/bin/cmake --build /tmp/minirpc-tdd -j2 && ctest --test-dir /tmp/minirpc-tdd -R "rpc_client_test|rpc_server_test" --output-on-failure'`

Expected: 两项测试 PASS，并发测试不挂起。

- [ ] **Step 9: 提交 Task 7**

```bash
git add code/MiniRPC/CMakeLists.txt code/MiniRPC/include/mini_rpc/rpc_client.h code/MiniRPC/src/rpc_client.cpp code/MiniRPC/tests/rpc_client_test.cpp
git commit -m "feat(minirpc): correlate synchronous client calls"
```

### Task 8: RpcController 与 RpcChannel 业务门面

**Files:**
- Create: `code/MiniRPC/include/mini_rpc/rpc_controller.h`
- Create: `code/MiniRPC/src/rpc_controller.cpp`
- Create: `code/MiniRPC/include/mini_rpc/rpc_channel.h`
- Create: `code/MiniRPC/src/rpc_channel.cpp`
- Create: `code/MiniRPC/tests/rpc_channel_test.cpp`
- Modify: `code/MiniRPC/CMakeLists.txt`

**Interfaces:**
- Consumes: `RpcClient::call(request, timeout)`。
- Produces: phase1 文档中的 RpcController 状态 API 和 `RpcChannel::callMethod()`。

- [ ] **Step 1: 写 Controller 状态转换测试**

断言默认 failed=false、code=0、timeout=3000ms；setFailed 后字段一致；reset 恢复默认错误但保留用户设置的 timeout；setTimeout 对零和负值抛 invalid_argument。

- [ ] **Step 2: 写 Channel 成功和错误映射测试**

使用真实 RpcServer/RpcClient。成功调用断言返回 true、response payload 正确、controller 未失败；UnknownService 断言返回 false、controller code=2001；慢调用使用 20ms timeout 断言 code=1003。

- [ ] **Step 3: 运行测试并确认 RED**

Run: `wsl bash -lc '/tmp/codex-cmake/bin/cmake --build /tmp/minirpc-tdd -j2 --target rpc_channel_test'`

Expected: 编译失败，提示 Controller/Channel 头文件不存在。

- [ ] **Step 4: 实现 RpcController**

提供 `reset/failed/errorCode/errorText/setFailed/setTimeout/timeout`。reset 清除错误状态但不重置调用方配置的 timeout，使一个 Controller 可以按同一超时配置重复使用。

- [ ] **Step 5: 实现 RpcChannel**

`callMethod()` 先 controller.reset，构造 request，把 controller.timeout 传给 client.call；error_code 非零时 setFailed 并返回 false，否则 move response.payload 到输出并返回 true。

- [ ] **Step 6: 运行 Channel GREEN**

Run: `wsl bash -lc '/tmp/codex-cmake/bin/cmake --build /tmp/minirpc-tdd -j2 && ctest --test-dir /tmp/minirpc-tdd -R "rpc_channel_test|rpc_client_test" --output-on-failure'`

Expected: 两项测试 PASS。

- [ ] **Step 7: 提交 Task 8**

```bash
git add code/MiniRPC/CMakeLists.txt code/MiniRPC/include/mini_rpc/rpc_controller.h code/MiniRPC/src/rpc_controller.cpp code/MiniRPC/include/mini_rpc/rpc_channel.h code/MiniRPC/src/rpc_channel.cpp code/MiniRPC/tests/rpc_channel_test.cpp
git commit -m "feat(minirpc): add controller and channel facade"
```

### Task 9: Calculator 示例、恶意帧、断连与完整验证

**Files:**
- Create: `code/MiniRPC/examples/calculator_server.cpp`
- Create: `code/MiniRPC/examples/calculator_client.cpp`
- Modify: `code/MiniRPC/tests/codec_test.cpp`
- Modify: `code/MiniRPC/tests/rpc_server_test.cpp`
- Modify: `code/MiniRPC/tests/rpc_client_test.cpp`
- Modify: `code/MiniRPC/CMakeLists.txt`
- Create: `code/MiniRPC/README.md`

**Interfaces:**
- Consumes: 所有 Phase 1 公共接口。
- Produces: 可运行示例、构建说明、sanitizer 开关和 phase1 验收证据。

- [ ] **Step 1: 增加剩余验收测试并确认新增断言失败**

补充：服务端收到非法 magic 后主动断开；客户端在业务 worker 完成前断开不会崩溃；接近 16 MiB payload 端到端返回；连续三帧一次 send 被全部处理。先运行测试，确认至少一个新增断言因行为缺失或测试目标未注册而失败。

- [ ] **Step 2: 实现满足新增测试的最小修正**

只修正测试揭示的 framing、断连或生命周期问题；每个修正保持 Codec 完整帧后消费、weak connection 发送和 loop 线程销毁不变量。

- [ ] **Step 3: 添加 Calculator 示例**

server 注册 `CalculatorService.Add`，payload 格式为两个十进制整数和一个空格；解析失败抛 invalid_argument，由 RpcServer 转为 ServerError。client 连接 `127.0.0.1:8080`，通过 RpcChannel 调用 `"10 20"`，成功打印 `30`，失败打印 code/message 并返回非零。

- [ ] **Step 4: 添加 README 和 sanitizer CMake 选项**

`MINIRPC_ENABLE_ASAN=ON` 添加 `-fsanitize=address -fno-omit-frame-pointer`；`MINIRPC_ENABLE_TSAN=ON` 添加 `-fsanitize=thread -fno-omit-frame-pointer`；两者同时开启时 CMake FATAL_ERROR。README 记录 Linux/WSL 构建、测试及两个示例命令。

- [ ] **Step 5: 运行 Debug 全量验证**

Run: `wsl bash -lc 'rm -rf /tmp/minirpc-debug-final && /tmp/codex-cmake/bin/cmake -S /mnt/d/my_project/Unix/code/MiniRPC -B /tmp/minirpc-debug-final -DCMAKE_BUILD_TYPE=Debug && /tmp/codex-cmake/bin/cmake --build /tmp/minirpc-debug-final -j2 && ctest --test-dir /tmp/minirpc-debug-final --output-on-failure'`

Expected: 配置、构建退出码 0，CTest 0 failures。

- [ ] **Step 6: 运行 ASan 全量验证**

Run: `wsl bash -lc 'rm -rf /tmp/minirpc-asan-final && /tmp/codex-cmake/bin/cmake -S /mnt/d/my_project/Unix/code/MiniRPC -B /tmp/minirpc-asan-final -DCMAKE_BUILD_TYPE=Debug -DMINIRPC_ENABLE_ASAN=ON && /tmp/codex-cmake/bin/cmake --build /tmp/minirpc-asan-final -j2 && ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ctest --test-dir /tmp/minirpc-asan-final --output-on-failure'`

Expected: 0 failures，无 AddressSanitizer/LeakSanitizer 报告。

- [ ] **Step 7: 运行 TSan 全量验证**

Run: `wsl bash -lc 'rm -rf /tmp/minirpc-tsan-final && /tmp/codex-cmake/bin/cmake -S /mnt/d/my_project/Unix/code/MiniRPC -B /tmp/minirpc-tsan-final -DCMAKE_BUILD_TYPE=Debug -DMINIRPC_ENABLE_TSAN=ON && /tmp/codex-cmake/bin/cmake --build /tmp/minirpc-tsan-final -j2 && TSAN_OPTIONS=halt_on_error=1 ctest --test-dir /tmp/minirpc-tsan-final --output-on-failure'`

Expected: 0 failures，无 ThreadSanitizer data race 报告；若 WSL 内核/TSan runtime 不兼容，保留完整错误并单独报告，不能用 Debug 结果替代。

- [ ] **Step 8: 检查 diff、需求覆盖和工作区边界**

Run: `git diff --check && git status --short && git diff --stat HEAD`

Expected: 无 whitespace error；仅 MiniReactor Client、MiniRPC 实现和本计划范围内文件属于本次工作，用户原有未提交文件保持原状态。

- [ ] **Step 9: 提交 Task 9**

```bash
git add code/MiniRPC/CMakeLists.txt code/MiniRPC/README.md code/MiniRPC/examples code/MiniRPC/tests
git commit -m "test(minirpc): complete phase one rpc validation"
```

- [ ] **Step 10: 最终需求逐项核验**

对照 `code/MiniRPC/docs/phase1.md` 第 22 节逐项记录：实现文件、对应测试、Debug/ASan/TSan 结果。任何未验证项明确标为未验证，不作完成声明。
