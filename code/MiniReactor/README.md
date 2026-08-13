# MiniReactor

一个基于 Linux `epoll` 的 Reactor TCP 网络框架，C++17，仿 muduo 设计。

已完成三个阶段：

- **Phase1**：单线程最小 Reactor（监听、接入、读写分发、输入/输出缓冲、Echo 示例）。
- **Phase2**：One Loop Per Thread 多线程模型（EventLoopThread/EventLoopThreadPool、
  轮询负载均衡、ET 模式、eventfd 跨线程唤醒、runInLoop 任务队列）。
- **Phase3**：集成上级目录（`code/`）的四个轮子工程，进一步对齐 muduo：

| 轮子 | 集成产物 |
|---|---|
| `AsyncLogger` | `base/Logger` 门面 + `MR_LOG_*` 宏，替换框架内全部 `cout/cerr` |
| `TimerScheduler` + `ThreadPool` | `base/TimerManager`（时间堆调度 + 工作窃取线程池） |
| `TimerManager` → `EventLoop` | `EventLoop::runAfter / runEvery / cancel`（回调 marshal 回 loop 线程） |
| `TcpConnection` | `forceClose / forceCloseWithDelay`（配合定时器做空闲超时） |
| `BlockingQueue` | `compute_server` 示例：IO 线程 ↔ 业务线程解耦（有界阻塞背压） |

详见 `docs/phase1.md`、`docs/phase2.md`、`docs/phase3.md`。

## 构建与运行

```bash
cmake -S . -B build
cmake --build build
./build/echo_server 8080          # Echo 服务器
./build/idle_echo_server 8080 5   # 空闲 5 秒自动断开的 Echo 服务器
./build/compute_server 8081 2     # 业务线程池计算服务器（字符串翻转）
```

另一个终端可用 `nc 127.0.0.1 8080` 连接，发送的内容会被原样回显。
日志默认写入 `logs/minireactor.log`（可在首次日志前用 `Logger::init(config)` 自定义）。

## 测试

```bash
ctest --test-dir build --output-on-failure
```

覆盖：Buffer 单元测试、EventLoopThread/线程池、Echo 端到端（2 worker 并发回显大负载）、
定时器（runAfter/runEvery/cancel/线程 marshal）、空闲超时端到端、日志落盘，
以及 AsyncLogger 轮子自带的单元测试。

## 说明

- 仅支持 Linux（依赖 epoll / eventfd / accept4 / MSG_NOSIGNAL）。
- 轮子以源码/子目录方式接入（`add_subdirectory(../AsyncLogger ...)` +
  `../ThreadPool`、`../TimerScheduler`、`../BlockingQueue` 加入构建）。
- `compute_server` 依赖的 `BlockingQueue` 使用 `std::span`，该示例目标为 C++20，
  框架库本身保持 C++17。
