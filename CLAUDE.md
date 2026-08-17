# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

本仓库是《Unix 环境高级编程》（APUE）学习仓库：`notes/` 存放学习笔记，`code/` 存放 C++ 服务端练习工程（"轮子"）。代码注释、docs、笔记均为中文，新代码沿用中文注释风格。

## 构建与测试

工程之间**没有顶层 CMakeLists**，每个工程独立配置。主要命令：

```bash
# MiniReactor（主项目：框架 + 全部集成测试 + AsyncLogger 自带测试）
cd code/MiniReactor
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure   # 全部测试
ctest --test-dir build -R echo_server_test   # 单个测试（-R 匹配 CTest 名称）

# AsyncLogger（自带库 target async_logger 与单元测试）
cd code/AsyncLogger
cmake -S . -B build && cmake --build build && ctest --test-dir build

# TimerScheduler / BlockingQueue
cd code/TimerScheduler && cmake -S . -B build && cmake --build build   # test_phase1/2/3
cd code/BlockingQueue && cmake -S . -B build && cmake --build build    # blocking_queue_test

# ThreadPool：CMake 根在 tests/（工程本身无根 CMakeLists.txt）
cd code/ThreadPool/tests
cmake -S . -B build && cmake --build build    # test_phase1、bench_phase31
```

注意事项：

- **仅支持 Linux**：MiniReactor 依赖 epoll / eventfd / accept4 / MSG_NOSIGNAL，必须在 Linux（或 WSL）上构建；当前开发机是 Windows，本机直接 cmake 会失败。
- ThreadPool / TimerScheduler / BlockingQueue 的 CMakeLists 硬性要求 GCC（`NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"` 时 `FATAL_ERROR`）。
- 严格告警：`-Wall -Wextra -Wpedantic`，部分工程加 `-Werror` 或 `-Wconversion -Wshadow`。
- 默认 C++17（`CMAKE_CXX_EXTENSIONS OFF`）；仅 `compute_server` 及其依赖的 BlockingQueue 为 C++20（`std::span`）。

## code/ 工程结构与开发模式

`code/` 下是层层递进的独立工程，MiniReactor 以"轮子"方式复用它们：

| 工程 | 定位 | 被 MiniReactor 集成方式 |
|---|---|---|
| `AsyncLogger` | 异步日志（有界 MPSC 队列 + 后台写线程 + 按大小滚动） | `add_subdirectory(../AsyncLogger ...)`，链接 `async_logger` |
| `ThreadPool` | 工作窃取线程池（全局有界环形队列 + 每线程本地队列，`MoveOnlyFunction` 带 SBO） | 源码引入 `../ThreadPool/ThreadPool.cpp` |
| `TimerScheduler` | 时间堆定时调度（独立调度线程，回调交 ThreadPool 执行） | 源码引入 `../TimerScheduler/TimerScheduler.cpp` |
| `BlockingQueue` | 有界阻塞队列（背压语义），头文件库 | 仅加入 include 路径 + `std::span`（C++20） |
| `MiniReactor` | 主项目：epoll Reactor TCP 框架（仿 muduo，已完成 phase 1-3） | — |
| `MiniRPC` | **当前进行中的下一阶段**：基于 MiniReactor 的 RPC。目前只有 `docs/phase1.md` 设计文档，尚无代码 | 规划依赖 MiniReactor + ThreadPool + AsyncLogger |

**开发模式**：每个工程先写 `docs/phaseN.md`（任务书：目标、接口签名、架构图、完成标准），再按阶段实现；测试按阶段拆为 `test_phase1/2/3`。开工新阶段前先读对应 docs。MiniRPC 是下一个实现目标，以 `docs/phase1.md`（含 Protocol / Codec / ServiceRegistry / RpcServer / RpcClient / PendingCall / RpcController / RpcChannel 模块划分与实现顺序）为准。

### MiniReactor 架构（仿 muduo：事件层 / 连接层 / 门面层 + 轮子层）

模块分布 `include/net/`、`include/base/`（实现于 `src/`）：

- 门面：`TcpServer` —— `setConnectionCallback` / `setMessageCallback` / `setThreadNum` / `start`
- 连接：`TcpConnection` —— 线程安全 `send()`、`forceClose()`、`forceCloseWithDelay()`（配合定时器做空闲超时）
- 事件：`EventLoop` —— `runInLoop` / `queueInLoop` / `runAfter` / `runEvery` / `cancel`
- 复用：`Poller` 接口 → `EpollPoller`（ET 模式）；`Channel` 绑定 fd+事件；`Acceptor` / `Socket` 处理监听
- 线程：`EventLoopThread`（One Loop Per Thread）、`EventLoopThreadPool`（轮询负载均衡，多 worker）
- base：`Logger`（AsyncLogger 门面 + `MR_LOG_*` 宏）、`TimerManager`（TimerScheduler + ThreadPool 封装，进程级单例）

关键语义（改动时务必保持）：

- **回调在 loop 线程执行**：`EventLoop::runAfter/runEvery` 底层走 `TimerManager`（定时回调实际在 ThreadPool worker 线程跑），EventLoop 内再用 `queueInLoop` 把回调 marshal 回 loop 线程。EventLoop 持有 `shared_ptr<atomic<bool>> timerAlive_` 防析构后在途定时回调触碰悬垂 `this`。
- **IO 线程不执行业务逻辑**：耗时任务提交到 `TimerManager::executor()` 暴露的线程池，或通过 `BlockingQueue` 交给业务线程（见 `compute_server`：IO 线程 push 满则阻塞获得背压，业务线程 pop 后直接 `connection->send(result)`）。
- **线程安全边界**：跨线程共享状态（如 `idle_echo_server` 的 `connections` / `lastMessageTime`）必须用 `std::mutex` 保护；`TcpConnection::send` 内部 `runInLoop` 保证任意线程调用安全。
- **析构安全**：`TimerManager` 成员顺序 `pool_` 先构造、`scheduler_` 先析构（保证调度线程先于执行池析构）。
- 日志：未 `init()` 时默认 `logs/minireactor.log`、Debug 级别起，随 `Logger::instance()` 惰性启动；`Logger::init` 须在首次日志前调用。

测试（`test/`，经 `add_test` 注册）：buffer / event_loop_thread / echo_server（端到端，2 worker 大负载回显）/ timer_manager（runAfter / runEvery / cancel / marshal）/ idle_timeout（端到端）/ logger（落盘）。

示例程序（可直接运行验证）：`echo_server`、`idle_echo_server [port] [idleSeconds]`、`compute_server [port] [threads]`，用 `nc 127.0.0.1 <port>` 连接。

### 轮子工程要点

- **ThreadPool**：提交语义 `try_post`（非阻塞）/ `post_wait`（阻塞）/ `post_for`（限时）/ `submit`（返回 future）。Worker 线程内提交进**本地队列**（不占全局容量），外部线程提交进全局队列；空闲 Worker 先本地 → 全局 → 窃取别的 Worker 尾部 → 休眠。不变量：取任务在锁内、执行在锁外；关闭后排空已入队任务（含本地队列）再退出；拒绝策略 `abort` / `caller_runs` / `discard`（锁外执行）。
- **TimerScheduler**：`schedule_after` / `schedule_every` / `cancel`，时间堆 + 独立调度线程；实现细节以 `docs/phase3.md` 为准（曾修过取消堆顶定时器导致忙等死锁的问题）。
- **AsyncLogger**：`log` / `flush` / `stop`（幂等且不丢已接收日志）；overflow 策略 `Block` / `DropNewest`；`max_file_size == 0` 禁用滚动。MiniReactor 侧入口是 `MR_LOG_*` 宏。
- **BlockingQueue**：仅头文件，C++20，用法见 `MiniReactor/example/ComputeServer.cpp`。

## notes/ 学习笔记

- 目录：`notes/md/`（Markdown）、`notes/xmind/`（XMind）。
- 命名：`APUE_第N章_主题.md`（如 `APUE_第3-4章_文件IO.md`），分册用 `_上` / `_下` 后缀。注意 README 里写的 `_学习总结` 后缀与现有文件不一致，以实际文件为准。
- 风格：模型优先（"不要背 API，要掌握模型"），从 C++ 服务端工程角度总结 APUE 知识；与 `code/` 工程实践互为印证。

## Git 约定

- 提交信息用 conventional commits：`feat(scope):` / `fix(scope):` / `chore:` / `docs:`，scope 如 `minireactor` / `timerscheduler`。
- 新工程先有 docs/ 任务书再写实现；脚手架/临时目录（如 My* 目录）完成后清理掉而非保留（见提交 "chore: remove My* scaffolding directories"）。
