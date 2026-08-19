# MiniRPC Phase3：高可用补全（自动重连 + 自动重试）

> 目标：在 MiniRPC Phase2（异步 RPC + Protobuf + 多实例服务发现 + Round-Robin 负载均衡）基础上，补齐多实例模式的两块"自愈"能力——**实例宕机后自动重连** 与 **调用失败后自动重试（故障转移）**，让 RPC 客户端从"能调用集群"升级为"集群可用性有保障"。

## 1. Phase3 动机：当前多实例模式的两个短板

Phase2 完成后，客户端已经能"发现一组实例 + 轮询选择 + 异步调用 + 超时控制"，但还有两个明显的半成品：

```text
短板 1：宕机实例不会自动复活
    RpcClient 断连后 Session 永远停在 disconnected，
    readyEndpoints 只会"避开"它，没有自动重连机制，
    实例恢复后必须手动重新 connect() 才能重新接入轮询池。

短板 2：请求失败不会换实例
    某次调用因网络中断 / 超时失败，直接返回错误给业务层，
    没有故障转移——而多实例存在的意义就是"一个挂了，请求打到另一个"。
```

Phase3 只解决这两个问题，对应生产 RPC 客户端最基本的自我修复能力：

```text
高可用骨架 = 服务发现（Phase2）
           + 负载均衡（Phase2）
           + 自动重连（Phase3）← 连接级恢复，让实例"复活"
           + 自动重试（Phase3）← 请求级补救，让调用"换人"
```

## 2. 本阶段不实现

保持克制，不加入：

- 熔断（Circuit Breaker）——依赖错误统计基础，留给 Phase4
- 限流 / 认证 / TLS / 压缩
- Tracing / Metrics 可观测性
- 流式 RPC / 协程 / HTTP/2 / 多协议
- etcd / Consul 注册中心接入
- 一致性哈希负载均衡
- 复杂连接池（每实例多连接）

## 3. 总体架构变化

```text
                    Client
                      │
                      ▼
                 RpcChannel / Stub
                      │
                      ▼
                  RpcClient
        ┌─────────────┼─────────────┐
        ▼             ▼             ▼
  ServiceDiscovery  LoadBalancer  Session 连接池（每 Endpoint 一个）
        │             │             │
        │             │       ┌─────┴─────────┐
        │             │       ▼               ▼
        │             │   connected session  断开 session
        │             │       │               │
        │             │       │               ▼
        │             │       │        重连调度器（指数退避）
        ▼             ▼       ▼               │
        └───────── 选择 Endpoint ── 发送 ──────┘
                      │
                      ▼
                 call() 重试循环（超时预算内换实例）
                      │
                      ▼
                  响应 / 最终错误
```

核心变化在 `RpcClient` 内部，`RpcServer` 无需改动。

## 4. 自动重连（Reconnect）

### 4.1 设计

每个 `Session` 增加重连状态，断连后由 EventLoop 定时器驱动重连：

```cpp
struct Session {
    Endpoint endpoint;
    std::unique_ptr<minireactor::TcpClient> tcpClient;
    bool connected{false};
    bool connecting{false};
    std::size_t reconnectAttempts{0};   // 已连续重连次数（成功时清零）
    bool reconnectScheduled{false};     // 是否已安排重连定时器
    minireactor::EventLoop::TimerId reconnectTimerId{0};
};
```

### 4.2 触发点

断连/连接失败（`onConnection` 非 Connected 分支、`onConnectionError`）时，若满足全部条件则调度重连：

```text
client 未 stopping
且 重连开关开启（reconnectEnabled_，connect() 打开、disconnect() 关闭）
且 该 session 未 connected、未 connecting、未重复调度
```

### 4.3 指数退避

```cpp
delay = min(kReconnectBaseDelay * 2^reconnectAttempts, kReconnectMaxDelay)
// 默认：base 1s，max 30s → 1s, 2s, 4s, 8s, 16s, 30s, 30s, ...
```

参数可配置（测试需要短退避）：

```cpp
void setReconnectPolicy(std::chrono::milliseconds baseDelay,
                        std::chrono::milliseconds maxDelay);
```

### 4.4 恢复

- 重连成功（`onConnection` kConnected）：`reconnectAttempts = 0`，`reconnectScheduled = false`；
- 重连失败：`++reconnectAttempts`，按新退避再次调度；
- 主动 `disconnect()` / 析构：取消所有 session 的挂起重连定时器（`loop_->cancel`）。

### 4.5 关键点

- 重连期间 `readyEndpoints` 自然过滤该 session，调用不会打过去；
- 实例恢复后自动重新接入轮询池，业务层无感；
- `TcpClient` 在 stop 后重新 `connect()` 是安全的（MiniReactor 已支持，见 commit `fix(minireactor): support safe client reconnects`）。

## 5. 自动重试（Retry / Failover）

### 5.1 位置：同步路径 `call()`

重试放在 `call()` 层（`asyncCall` 保持"一次尝试"语义，异步调用者自行决定重试）：

```cpp
RpcResponse RpcClient::call(RpcRequest request, std::chrono::milliseconds timeout) {
    if (maxRetries_ == 0) {
        return asyncCall(std::move(request), timeout).get();   // 与 Phase2 一致
    }
    const auto deadline = steady_clock::now() + timeout;
    RpcResponse lastResponse;
    for (std::size_t attempt = 0; attempt <= maxRetries_; ++attempt) {
        const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now());
        if (remaining <= milliseconds::zero()) {              // 预算耗尽，立即失败
            return lastResponse;
        }
        // 剩余预算均分给剩余尝试次数（详见 5.3）
        const auto attemptsLeft = static_cast<std::int64_t>(maxRetries_ - attempt + 1);
        const auto attemptTimeout = std::max(milliseconds(1), remaining / attemptsLeft);
        lastResponse = asyncCall(request, attemptTimeout).get();
        if (!isRetryable(lastResponse.error_code)) {          // 不可重试，直接返回
            return lastResponse;
        }
        // 可重试：循环重发。每次 selectConnection 重新 discover + 轮询，天然换实例。
    }
    return lastResponse;
}
```

### 5.2 可重试错误分类

```cpp
bool isRetryable(int errorCode) {
    switch (static_cast<RpcErrorCode>(errorCode)) {
        case RpcErrorCode::NetworkError:
        case RpcErrorCode::Timeout:
            return true;               // 连接中断 / 超时 → 换实例重试
        default:
            return false;              // Protocol/MethodNotFound/ServiceUnavailable 等不重试
    }
}
```

设计说明：

- **`ServiceUnavailable` 不重试**：无可用实例时重试无意义（除非有新实例注册），立即返回；
- **`ProtocolError` / `ServiceNotFound` / `MethodNotFound` 不重试**：是确定性错误，重试必然再失败；
- **`ServerError` 不重试**：业务方法抛出的错误，默认视为确定性（可配置扩展）。

### 5.3 超时预算（Time Budget）

关键设计：**总等待时间不超过调用方给的 `timeout`**。

```text
错误做法：单次超时 × 重试次数 → 一次调用可能卡 N 倍超时
正确做法：deadline = now + timeout，每次尝试只允许"剩余预算的均分份额"
```

实现细节：`deadline = now + timeout`，第 `attempt` 次尝试的允许超时为

```text
attemptTimeout = max(1ms, remaining / (maxRetries - attempt + 1))
```

即剩余预算均分给剩余尝试次数。这样：

- **总耗时 ≤ timeout**：无论重试多少次，预算不翻倍；
- **超时类失败也有重试机会**：第一次尝试分到 `timeout / (maxRetries+1)`，
  失败后仍有预算换实例；
- **`setMaxRetries(0)` 时完全等价于 Phase2 的单次调用**（第一次尝试 = 完整 timeout）。

注意权衡：开启重试后，单实例场景下每次尝试的超时会变短
（例如 `maxRetries=1` 时第一次只有 `timeout/2`）。这是"用单次超时换取失败转移"的
显式取舍，调用方可通过 `setMaxRetries(0)` 关闭。

### 5.4 配置

```cpp
void setMaxRetries(std::size_t maxRetries);   // 默认 2（共 3 次尝试）；0 = 不重试
```

### 5.5 幂等性说明（教学重点）

`Timeout` 可能意味着服务端已执行（响应丢失/迟到），重试写操作有重复执行风险。调用方应只为**幂等方法**开启重试，或在业务层做去重。本阶段不实现请求去重，仅在文档中明确该约束。

## 6. 接口变更汇总

| 位置 | 变更 |
|---|---|
| `RpcClient` | 新增 `setMaxRetries(size_t)`、`setReconnectPolicy(ms, ms)`、`setReconnectEnabled(bool)` |
| `RpcClient::Session` | 新增重连状态字段（attempts / scheduled / timerId） |
| `RpcClient::call` | 内部增加重试循环（签名不变，向后兼容） |
| `RpcServer` | 无改动 |
| `protocol.h` | 无改动（复用现有错误码） |

## 7. 实现顺序

```text
1. Session 增加重连字段 + 退避工具函数
2. onConnection / onConnectionError 接入重连调度
3. disconnect / 析构取消挂起重连
4. 重连单测（短退避：关 server → 起 server → 自动恢复）
5. call() 增加重试循环 + isRetryable 分类
6. 重试单测（慢实例超时 → 换快实例成功；预算耗尽）
7. WSL 全量 ctest + ASan 验证
```

## 8. 测试方案

### 重连（`rpc_reconnect_test.cpp`）

```text
Phase A：server1 起在端口 P，client connect 成功
Phase B：server1 关闭 → client 感知断连 → 退避调度（测试用 100ms）
Phase C：server2 起在相同端口 P → client 自动重连成功 → call 恢复
断言：Phase C 的 call 成功返回
```

### 重试（`rpc_retry_test.cpp`）

```text
用例 1（重试成功）：实例 A 方法慢（2s）+ 实例 B 方法快；
     timeout 300ms、maxRetries 1 → 第一次（A）超时 → 重试命中 B 成功。
用例 2（预算生效）：A、B 都慢（2s）；timeout 500ms、maxRetries 1
     → 两次都超时，总耗时约 500ms（而非 1000ms+），返回 Timeout。
用例 3（不可重试）：MethodNotFound 只发一次，立即返回，不重试。
```

### 回归

现有 11 个测试必须全绿（重连/重试对既有单实例与多实例语义无破坏）。

## 9. Phase3 完成标准

- [ ] 断连后按指数退避自动重连，实例恢复后自动重新接入轮询池
- [ ] 主动 disconnect() 后不再重连
- [ ] `call()` 对 NetworkError / Timeout 自动换实例重试
- [ ] 重试受超时预算约束，总耗时不超过调用方 timeout
- [ ] 不可重试错误（Protocol/MethodNotFound/ServiceUnavailable）不重试
- [ ] `setMaxRetries(0)` 关闭重试，行为与 Phase2 一致
- [ ] 重连 / 重试测试通过
- [ ] 原有 11 个测试全部通过
- [ ] WSL（cmake 3.16）构建 + ctest 全绿

## 10. 收尾后的认识

Phase3 完成后，MiniRPC 的多实例调用具备完整的自我修复闭环：

```text
发现（实例在哪）→ 均衡（这次选谁）→ 重连（挂了自动复活）→ 重试（失败自动换人）
```

下一阶段（Phase4）再考虑基于错误统计的熔断、限流与可观测性。
