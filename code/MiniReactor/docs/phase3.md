# MiniReactor Phase3：轮子工程集成

## 目标

上一级目录（`code/`）已有四个独立的"轮子"工程：

| 轮子 | 定位 | 集成方式 |
|---|---|---|
| `AsyncLogger` | 异步日志（MPSC 环形队列 + 后台写线程 + 文件滚动） | `add_subdirectory`，链接 `async_logger` 库 |
| `TimerScheduler` | 定时任务调度（时间堆 + 独立调度线程） | 源码 `TimerScheduler.cpp` 引入 |
| `ThreadPool` | 工作窃取线程池（有界环形队列 + 本地队列） | 源码 `ThreadPool.cpp` 引入 |
| `BlockingQueue` | 有界阻塞队列（背压语义） | 仅头文件，加入 include 路径 |

Phase3 把这四个轮子接入 MiniReactor，使框架向 muduo 对齐：

- **定时器**：`EventLoop::runAfter / runEvery / cancel`（muduo 核心能力，原 Phase2 遗留项）；
- **异步日志**：`Logger` 门面 + `MR_LOG_*` 宏，替换框架内全部 `std::cout / std::cerr`；
- **业务线程池**：`TimerManager::executor()` 暴露工作窃取线程池，IO 与业务解耦；
- **阻塞队列**：`ComputeServer` 示例演示 IO 线程与业务线程通过有界队列解耦。

## 新增模块

### 1. `base/Logger`（AsyncLogger 门面）

```cpp
minireactor::Logger::init(config);   // 可选，须在第一次日志前调用
MR_LOG_INFO("acceptor listening on " + std::to_string(port));
```

- 未 `init()` 时使用默认配置：`logs/minireactor.log`，`Debug` 级别起；
- 全局单例，`instance()` 首次调用时惰性构造并启动后台写线程；
- 宏：`MR_LOG_TRACE/DEBUG/INFO/WARN/ERROR/FATAL`。

### 2. `base/TimerManager`（TimerScheduler + ThreadPool 封装）

```cpp
TimerManager& timers = TimerManager::instance();   // 进程级单例，首次调度自动启动
TimerManager::TimerId id = timers.runAfter(5s, task);
TimerManager::TimerId id = timers.runEvery(1s, task);
timers.cancel(id);
ThreadPool& pool = timers.executor();              // 业务任务共用线程池
```

- 成员顺序保证析构安全：`ThreadPool pool_` 先构造、`TimerScheduler scheduler_` 先析构；
- 定时回调在 ThreadPool worker 线程执行；MiniReactor 层再做线程 marshal。

### 3. `EventLoop` 定时器（muduo 风格）

```cpp
EventLoop::TimerId id = loop.runAfter(seconds, callback);
EventLoop::TimerId id = loop.runEvery(seconds, callback);
loop.cancel(id);
```

- 底层走 `TimerManager::instance()`；回调经 `queueInLoop` **marshal 回 loop 线程**执行，
  与 muduo "回调在 loop 线程" 语义一致（同时不阻塞 epoll_wait）；
- `EventLoop` 持有 `shared_ptr<atomic<bool>> timerAlive_`：析构时先失效标志再统一
  `cancel`，防止在途定时回调触碰悬垂 `this`。

### 4. `TcpConnection` 主动断开

```cpp
connection->forceClose();               // 立即断开（runInLoop → handleClose）
connection->forceCloseWithDelay(0.1);   // 定时器延迟断开（runAfter + forceClose）
```

配合 `EventLoop::runEvery` 即可实现 muduo 经典的"空闲连接超时断开"。

## 示例

### `idle_echo_server`（空闲超时断开，muduo idleconnection 风格）

```bash
./idle_echo_server [port] [idleSeconds]   # 默认 8080 / 5
```

每秒检查一次连接的最后消息时间，超时则 `forceCloseWithDelay(0.1)` 断开。
注意：`messageCallback` 运行在 worker loop 线程、检查回调运行在主 loop 线程，
共享的 `connections` / `lastMessageTime` 用 `std::mutex` 保护。

### `compute_server`（BlockingQueue 业务池演示）

```bash
./compute_server [port] [businessThreads]   # 默认 8081 / 2
```

IO 线程把任务 `push` 进有界 `BlockingQueue<ComputeTask>`（满则阻塞，背压）；
业务线程 `pop` 处理后在**业务线程**里直接 `connection->send(result)`——
`send` 内部 `runInLoop` 保证线程安全；连接断开后任务经 `weak_ptr.lock()` 自然丢弃。

## 测试

| 测试 | 覆盖 |
|---|---|
| `timer_manager_test` | runAfter 一次性、runEvery 周期 + cancel、触发前 cancel、EventLoop 定时器 marshal 到 loop 线程、runEvery/cancel |
| `idle_timeout_test` | 端到端：多 worker 下空闲连接被 `forceCloseWithDelay` 主动断开 |
| `logger_test` | MR_LOG_* 六级日志写入 + flush 计数 + 文件落盘 |

另：`add_subdirectory(../AsyncLogger ...)` 同时带入 AsyncLogger 自身单元测试，
`ctest` 统一执行。

## 架构总览

```
业务层   EchoServer / IdleEchoServer / ComputeServer
        │
门面层   TcpServer ── setConnectionCallback / setMessageCallback
        │
连接层   TcpConnection（forceClose / forceCloseWithDelay / send 线程安全）
        │
事件层   EventLoop ── runInLoop / runAfter / runEvery / cancel ──┐
        │                                                       │ marshal
复用层   Poller → EpollPoller                                   │
线程层   EventLoopThread / EventLoopThreadPool                   │
        ┌────────────────────────────────────────────────────────┘
        ▼
轮子层   TimerManager ── TimerScheduler（时间堆）
              │
              └── ThreadPool（回调执行 + 业务任务）
        AsyncLogger（后台写线程 + MPSC 队列 + 文件滚动） ← Logger 门面
        BlockingQueue（ComputeServer 业务队列）
```

## 与 muduo 的差距（后续可做）

- `Channel::tie()` 生命周期保护；
- `InetAddress`、`TcpConnection::writeCompleteCallback / highWaterMarkCallback`；
- `Connector / TcpClient`（客户端侧 + 指数退避重连）；
- `Buffer::readFd(readv)` 与协议解析辅助（`findCRLF`、`peekInt` 等）；
- `TimerQueue` 直接挂进 EventLoop（timerfd 方案，省去线程 marshal）。
