# 异步日志库：第一版一次性实现任务书

> 目标：一次性实现一个**功能完整、线程安全、可正常关闭、可用于服务端项目练习**的异步日志库。  
> 本版本优先保证正确性、接口清晰和生命周期可靠，不追求极致性能。

---

## 1. 项目目标

实现一个 C++17 异步日志库，使业务线程只负责构造并提交日志，后台日志线程负责将日志写入文件。

整体流程：

```text
多个业务线程
    │
    │ log()
    ▼
线程安全有界队列
    │
    │ condition_variable 通知
    ▼
后台日志线程
    │
    ├── 格式化日志
    ├── 写入文件
    ├── 定期刷新
    └── 文件滚动
```

第一版应具备以下能力：

- 支持多个线程并发写日志。
- 支持日志级别过滤。
- 支持异步文件写入。
- 支持有界日志队列。
- 支持队列满时的明确处理策略。
- 支持手动 `flush()`。
- 支持安全、幂等的 `stop()`。
- 析构时自动停止，并写完已经接收的日志。
- 支持按文件大小滚动日志。
- 能够统计被丢弃的日志数量。
- 日志线程内部发生错误时，不导致程序直接崩溃。

---

## 2. 本版本不实现的内容

以下内容暂时不做，后续可以作为性能优化版本继续实现：

- 无锁队列。
- SPSC、MPSC、MPMC 专用队列。
- 双缓冲或多缓冲。
- 每线程 TLS 日志缓冲区。
- 固定长度日志缓冲区。
- 小对象优化。
- `writev` 批量写入。
- mmap 日志文件。
- 时间戳字符串缓存。
- 自定义高性能整数转字符串。
- 编译期格式化。
- 网络日志、远程日志上报。
- 日志压缩和后台清理线程。
- 多个日志消费者线程。

第一版使用：

```text
std::mutex
std::condition_variable
std::deque<LogRecord>
std::ofstream
单个后台工作线程
```

---

## 3. 建议目录结构

```text
async_logger/
├── CMakeLists.txt
├── include/
│   └── async_logger/
│       ├── async_logger.h
│       ├── log_level.h
│       └── log_record.h
├── src/
│   ├── async_logger.cpp
│   └── log_level.cpp
├── examples/
│   └── basic_example.cpp
└── tests/
    └── async_logger_test.cpp
```

职责划分：

| 文件 | 职责 |
|---|---|
| `log_level.h` | 定义日志级别及字符串转换 |
| `log_record.h` | 定义一条日志记录的数据结构 |
| `async_logger.h` | 对外接口、配置结构、成员变量 |
| `async_logger.cpp` | 队列、线程、文件写入、刷新和滚动实现 |
| `basic_example.cpp` | 展示基本使用方式 |
| `async_logger_test.cpp` | 并发、停止、刷新和滚动测试 |

---

## 4. 日志级别

定义以下日志级别：

```cpp
enum class LogLevel {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};
```

需要提供：

```cpp
std::string_view to_string(LogLevel level) noexcept;
```

级别过滤规则：

```text
record.level < config.min_level
    → 直接忽略
    → 不进入队列
```

例如最低级别为 `Info` 时，`Trace` 和 `Debug` 日志不会被接收。

---

## 5. 日志记录结构

建议定义：

```cpp
struct LogRecord {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    std::thread::id thread_id;
    std::string file;
    int line;
    std::string function;
    std::string message;
    std::uint64_t sequence;
};
```

各字段含义：

- `timestamp`：业务线程调用日志接口时的时间。
- `level`：日志级别。
- `thread_id`：产生日志的业务线程。
- `file`：源文件名。
- `line`：源代码行号。
- `function`：函数名。
- `message`：日志正文。
- `sequence`：日志库内部递增序号，用于 `flush()` 和测试。

第一版允许 `file`、`function` 和 `message` 产生字符串分配，不需要提前做固定缓冲区优化。

---

## 6. 配置结构

建议定义：

```cpp
enum class OverflowPolicy {
    Block,
    DropNewest
};

struct LoggerConfig {
    std::filesystem::path file_path;
    LogLevel min_level = LogLevel::Info;

    std::size_t max_queue_size = 8192;
    OverflowPolicy overflow_policy = OverflowPolicy::Block;

    std::chrono::milliseconds flush_interval{1000};

    std::uintmax_t max_file_size = 100 * 1024 * 1024;
    std::size_t max_backup_files = 5;

    bool flush_on_error = false;
};
```

配置要求：

### `file_path`

日志文件路径，例如：

```text
logs/server.log
```

启动时应自动创建父目录。

### `max_queue_size`

队列必须有上限，禁止日志积压时无限占用内存。

要求：

```text
max_queue_size > 0
```

否则构造或启动时抛出 `std::invalid_argument`。

### `OverflowPolicy::Block`

队列满时，业务线程阻塞，直到：

- 队列出现空位；
- 日志库进入停止状态；
- 日志线程发生不可恢复错误。

优点是尽量不丢日志，缺点是极端情况下会影响业务线程延迟。

### `OverflowPolicy::DropNewest`

队列满时直接丢弃当前日志，并增加丢弃计数。

接口应返回 `false`，使调用方能够知道本条日志没有被接收。

### `flush_interval`

后台线程即使没有接收到显式 `flush()`，也需要定期刷新文件流。

### `max_file_size`

当前日志文件达到该大小后进行滚动。

设置为 `0` 时可表示禁用文件滚动。

### `max_backup_files`

保留的历史日志数量。

例如：

```text
server.log
server.log.1
server.log.2
server.log.3
```

其中 `.1` 是最近一次滚动产生的文件。

---

## 7. 对外接口

建议接口如下：

```cpp
class AsyncLogger {
public:
    explicit AsyncLogger(LoggerConfig config);
    ~AsyncLogger();

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;
    AsyncLogger(AsyncLogger&&) = delete;
    AsyncLogger& operator=(AsyncLogger&&) = delete;

    void start();

    bool log(LogLevel level,
             std::string message,
             const char* file,
             int line,
             const char* function);

    void flush();
    void stop();

    bool is_running() const noexcept;
    bool has_error() const noexcept;

    std::uint64_t accepted_count() const noexcept;
    std::uint64_t written_count() const noexcept;
    std::uint64_t dropped_count() const noexcept;
};
```

### 生命周期约束

采用显式启动：

```cpp
AsyncLogger logger(config);
logger.start();
```

原因：

- 构造函数只负责建立对象不变量。
- 文件打开和线程创建等可能失败的操作放在 `start()` 中。
- 生命周期更加明确。
- 更容易测试启动失败场景。

要求：

- 重复调用 `start()` 应抛出 `std::logic_error`。
- 重复调用 `stop()` 必须安全，不得重复 `join()`。
- 析构函数内部调用 `stop()`。
- 析构函数不得抛异常。

---

## 8. 推荐日志宏

为了自动记录源文件、行号和函数名，可以提供宏：

```cpp
#define ASYNC_LOG(logger, level, message) \
    (logger).log((level), (message), __FILE__, __LINE__, __func__)

#define LOG_TRACE(logger, message) \
    ASYNC_LOG((logger), LogLevel::Trace, (message))

#define LOG_DEBUG(logger, message) \
    ASYNC_LOG((logger), LogLevel::Debug, (message))

#define LOG_INFO(logger, message) \
    ASYNC_LOG((logger), LogLevel::Info, (message))

#define LOG_WARN(logger, message) \
    ASYNC_LOG((logger), LogLevel::Warn, (message))

#define LOG_ERROR(logger, message) \
    ASYNC_LOG((logger), LogLevel::Error, (message))

#define LOG_FATAL(logger, message) \
    ASYNC_LOG((logger), LogLevel::Fatal, (message))
```

第一版不要求实现 `{}` 占位符格式化。

调用方自行构造字符串：

```cpp
LOG_INFO(logger, "server started");

LOG_ERROR(
    logger,
    "failed to process request, request_id=" + request_id);
```

---

## 9. 内部状态

建议定义：

```cpp
enum class State {
    Created,
    Running,
    Stopping,
    Stopped,
    Failed
};
```

状态含义：

```text
Created
  │ start()
  ▼
Running
  │ stop()
  ▼
Stopping
  │ 队列清空、文件刷新、线程退出
  ▼
Stopped
```

异常路径：

```text
Running
  │ 后台线程出现不可恢复错误
  ▼
Failed
```

基本约束：

- 只有 `Running` 状态允许接收新日志。
- `Stopping` 状态不再接收新日志，但必须处理完已经接收的日志。
- `Stopped` 状态不能重新启动。
- `Failed` 状态不再接收日志。
- `stop()` 对 `Created`、`Stopping`、`Stopped` 和 `Failed` 都必须安全。

为了简化同步，状态本身可以由同一个互斥锁保护，不必为了状态额外引入复杂的原子协议。

---

## 10. 核心成员变量

建议成员如下：

```cpp
LoggerConfig config_;

mutable std::mutex mutex_;
std::condition_variable data_cv_;
std::condition_variable space_cv_;
std::condition_variable flush_cv_;

std::deque<LogRecord> queue_;
std::thread worker_;

State state_ = State::Created;

std::ofstream output_;
std::uintmax_t current_file_size_ = 0;

std::uint64_t next_sequence_ = 0;
std::uint64_t last_written_sequence_ = 0;

std::atomic<std::uint64_t> accepted_count_{0};
std::atomic<std::uint64_t> written_count_{0};
std::atomic<std::uint64_t> dropped_count_{0};
std::atomic<bool> has_error_{false};
```

三个条件变量分别负责：

| 条件变量 | 用途 |
|---|---|
| `data_cv_` | 通知日志线程队列中有新日志或需要停止 |
| `space_cv_` | 通知阻塞的生产者队列出现空位 |
| `flush_cv_` | 通知等待 `flush()` 的线程目标日志已经写入 |

---

## 11. `log()` 的行为

`log()` 应完成以下步骤：

```text
1. 检查日志级别
2. 构造 LogRecord
3. 获取 mutex_
4. 检查状态是否为 Running
5. 检查队列是否已满
6. 根据过载策略阻塞或丢弃
7. 分配 sequence
8. 将记录移动进队列
9. 增加 accepted_count
10. 释放锁
11. data_cv_.notify_one()
12. 返回 true
```

### 级别过滤

日志级别不满足要求时，可以直接返回 `true`。

这里的含义是：

```text
该日志被配置主动过滤，而不是因为系统过载丢失。
```

也可以单独增加 `filtered_count`，但第一版不是必须要求。

### 状态检查

当状态不为 `Running` 时：

```cpp
return false;
```

不允许在 `stop()` 开始后继续向队列中添加日志。

### 阻塞策略

使用带谓词的等待：

```cpp
space_cv_.wait(lock, [&] {
    return queue_.size() < config_.max_queue_size ||
           state_ != State::Running;
});
```

被唤醒后必须再次检查状态。

禁止使用无谓词的裸 `wait()` 后直接假设条件成立。

---

## 12. 后台线程循环

后台线程核心逻辑：

```text
while (true) {
    等待以下任一条件：
        1. 队列非空
        2. 到达定期刷新时间
        3. 状态不再是 Running

    如果队列非空：
        取出队首日志
        通知一个等待队列空间的生产者
        解锁

        检查是否需要滚动文件
        格式化日志
        写入文件
        更新 written_count 和 last_written_sequence

        重新加锁
        通知 flush_cv_

    如果到达刷新周期：
        解锁
        output_.flush()

    如果状态为 Stopping 且队列为空：
        解锁
        output_.flush()
        退出循环
}
```

推荐使用 `wait_until()` 或 `wait_for()` 同时处理日志到达和周期刷新。

退出条件必须是：

```text
正在停止 && 队列已经为空
```

不能只因为收到停止请求就立即退出，否则会丢失已经接收但尚未写入的日志。

---

## 13. 日志格式

推荐输出格式：

```text
2026-07-29 22:26:50.123456 [INFO] [tid=18432] [server.cpp:87 process_request] request completed
```

字段顺序：

```text
时间
日志级别
线程 ID
源文件与行号
函数名
日志正文
换行
```

格式示例：

```text
2026-07-29 22:26:50.123456 [INFO] [tid=140735] [main.cpp:25 main] server started
2026-07-29 22:26:51.002317 [ERROR] [tid=140812] [handler.cpp:91 query] database timeout
```

要求：

- 每条日志最终必须以 `\n` 结束。
- 日志正文中如果包含换行符，第一版可以原样保留。
- 时间使用本地时间。
- 至少保留毫秒，推荐保留微秒。
- 文件路径可以只保留文件名，也可以保留完整路径；两者选择一种并保持一致。

---

## 14. `flush()` 语义

`flush()` 必须满足：

> `flush()` 返回时，调用 `flush()` 之前已经被日志库接收的日志，必须已经写入文件流，并执行过 `output_.flush()`。

推荐实现：

```text
1. 获取 mutex_
2. 记录 target_sequence = next_sequence_
3. 通知日志线程
4. 等待 last_written_sequence_ >= target_sequence
5. 让日志线程或当前线程执行 output_.flush()
6. 返回
```

需要处理的情况：

- 队列为空。
- 日志库未启动。
- 日志库正在停止。
- 日志线程已经失败。
- 多个线程同时调用 `flush()`。

为了避免多个线程直接并发操作 `std::ofstream`，建议只有日志线程执行文件写入和刷新。

可以增加：

```cpp
std::uint64_t flush_target_sequence_ = 0;
bool flush_requested_ = false;
```

调用线程提交刷新请求，日志线程完成刷新后再通知等待者。

---

## 15. `stop()` 语义

`stop()` 的正确流程：

```text
1. 获取 mutex_
2. 如果已经 Stopping、Stopped 或 Failed，按状态安全返回
3. 如果仍为 Created，直接切换为 Stopped
4. 如果为 Running，切换为 Stopping
5. 释放锁
6. 唤醒 data_cv_、space_cv_ 和 flush_cv_
7. 如果 worker_ 可 join，则 join()
8. 关闭文件
9. 将状态设置为 Stopped
```

核心保证：

- `stop()` 一旦开始，不再接收新日志。
- 已经返回 `true` 的日志必须尽量全部写入。
- 所有阻塞在队列空间上的生产者都必须被唤醒。
- 不能在持有 `mutex_` 时调用 `worker_.join()`，否则可能死锁。
- 多线程并发调用 `stop()` 时，只能有一个线程真正执行 `join()`。

建议增加单独的生命周期互斥锁：

```cpp
std::mutex lifecycle_mutex_;
```

由它串行化 `start()` 和 `stop()`，而 `mutex_` 只保护队列和运行状态。

这样可以避免两个线程同时判断 `worker_.joinable()` 并同时调用 `join()`。

---

## 16. 文件打开与目录创建

`start()` 时：

```text
1. 校验配置
2. 创建日志文件父目录
3. 以 append 模式打开日志文件
4. 获取当前文件大小
5. 创建后台线程
6. 状态切换为 Running
```

文件打开模式建议：

```cpp
std::ios::out | std::ios::app
```

如果目录创建或文件打开失败：

```cpp
throw std::runtime_error(...);
```

线程不能在文件尚未成功打开时启动。

---

## 17. 文件滚动

采用按大小滚动。

假设：

```text
file_path = server.log
max_backup_files = 3
```

滚动前：

```text
server.log
server.log.1
server.log.2
server.log.3
```

滚动流程：

```text
1. flush 并关闭 server.log
2. 删除 server.log.3
3. server.log.2 → server.log.3
4. server.log.1 → server.log.2
5. server.log   → server.log.1
6. 创建新的 server.log
7. current_file_size_ 归零
```

写入一条日志之前判断：

```text
current_file_size_ + formatted_log.size() > max_file_size
```

满足时先滚动，再写入当前日志。

边界规则：

- `max_file_size == 0`：禁用滚动。
- `max_backup_files == 0`：达到限制时直接截断或重新创建当前文件。
- 单条日志本身大于文件上限：允许该条日志独占一个超大文件，不循环滚动。
- 文件重命名失败时记录错误状态，并将错误写到 `std::cerr`。

第一版只允许后台日志线程操作日志文件和执行文件滚动，不需要额外文件锁。

---

## 18. 错误处理

### 业务线程

`log()` 不应因为正常的队列满或停止状态抛异常：

```text
接收成功      → true
队列满并丢弃  → false
日志库已停止  → false
后台线程失败  → false
```

日志记录构造时的内存分配异常可以自然向上传播，第一版不要求完全 `noexcept`。

### 后台线程

后台线程入口必须捕获异常：

```cpp
try {
    worker_loop();
} catch (const std::exception& ex) {
    // 设置 Failed，记录错误，唤醒所有等待线程
} catch (...) {
    // 同样处理未知异常
}
```

发生不可恢复错误时：

```text
state_ = Failed
has_error_ = true
```

并通知：

```cpp
data_cv_.notify_all();
space_cv_.notify_all();
flush_cv_.notify_all();
```

后台线程不能让异常穿透线程入口，否则会调用 `std::terminate()`。

### 错误兜底

日志文件写入失败时，可以向 `std::cerr` 输出一条简短错误信息。

不要尝试把“日志系统自身的错误”再次写入同一个异步日志库，否则容易递归。

---

## 19. 计数指标

提供以下统计值：

```text
accepted_count
written_count
dropped_count
```

定义：

### `accepted_count`

成功进入队列的日志数量。

### `written_count`

后台线程成功完成文件写入的日志数量。

### `dropped_count`

由于队列满且策略为 `DropNewest` 而丢失的日志数量。

正常停止后，在没有文件错误的情况下应满足：

```text
accepted_count == written_count
```

如果使用 `DropNewest`：

```text
日志调用总数
    = 被级别过滤的数量
    + accepted_count
    + dropped_count
    + 因停止或失败而拒绝的数量
```

---

## 20. 线程安全不变量

实现过程中必须始终保持以下不变量：

1. `queue_` 只能在持有 `mutex_` 时访问。
2. `state_` 的读写必须受统一同步机制保护。
3. `std::ofstream` 只能由后台线程操作。
4. 不能持有 `mutex_` 执行文件 I/O。
5. 不能持有 `mutex_` 调用 `join()`。
6. 队列大小不能超过 `max_queue_size`。
7. `Stopping` 状态不能再插入新日志。
8. 工作线程退出前必须处理完队列中已经接收的日志。
9. 条件变量等待必须使用谓词或循环检查条件。
10. 析构函数不得抛出异常。

---

## 21. 推荐实现顺序

虽然本任务不拆分交付阶段，但编码时建议按照以下依赖顺序完成：

```text
LogLevel
  ↓
LogRecord
  ↓
LoggerConfig
  ↓
文件打开与格式化
  ↓
线程安全队列
  ↓
后台线程
  ↓
start / log / stop
  ↓
flush
  ↓
文件滚动
  ↓
统计与错误状态
  ↓
并发测试
```

这只是编码顺序，最终应一次性交付完整可运行版本。

---

## 22. 使用示例

```cpp
#include "async_logger/async_logger.h"

int main() {
    LoggerConfig config;
    config.file_path = "logs/server.log";
    config.min_level = LogLevel::Debug;
    config.max_queue_size = 4096;
    config.overflow_policy = OverflowPolicy::Block;
    config.flush_interval = std::chrono::milliseconds(500);
    config.max_file_size = 10 * 1024 * 1024;
    config.max_backup_files = 3;

    AsyncLogger logger(std::move(config));
    logger.start();

    LOG_INFO(logger, "server started");

    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&logger, i] {
            for (int j = 0; j < 10000; ++j) {
                LOG_DEBUG(
                    logger,
                    "worker=" + std::to_string(i) +
                    ", index=" + std::to_string(j));
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    logger.flush();
    logger.stop();

    return logger.has_error() ? 1 : 0;
}
```

---

## 23. 必测场景

### 23.1 基础写入

写入若干日志后调用：

```cpp
flush();
stop();
```

检查日志文件内容、日志级别和行数。

### 23.2 日志级别过滤

最低级别设为 `Warn`，确认：

- `Trace`、`Debug`、`Info` 不写入。
- `Warn`、`Error`、`Fatal` 正常写入。

### 23.3 多线程并发

启动多个线程，每个线程写固定数量日志。

停止后检查：

```text
accepted_count == written_count
文件日志行数 == written_count
```

### 23.4 队列满时阻塞

使用极小队列并人为降低写入速度，验证：

- 队列大小不越界。
- 生产者能够阻塞。
- 后台线程消费后生产者可以继续。
- 调用 `stop()` 后阻塞生产者能够退出。

### 23.5 队列满时丢弃

使用 `DropNewest`，确认：

- `log()` 能返回 `false`。
- `dropped_count` 增加。
- 进程内存不会持续增长。

### 23.6 `flush()` 语义

提交一批日志后立即 `flush()`，不调用 `stop()`，读取文件确认日志已经可见。

### 23.7 安全停止

提交大量日志后立即调用 `stop()`，确认所有已经接收的日志都被写入。

### 23.8 重复停止

连续调用多次：

```cpp
logger.stop();
logger.stop();
logger.stop();
```

程序不能崩溃或死锁。

### 23.9 析构自动停止

不显式调用 `stop()`，离开作用域后检查日志是否完整。

### 23.10 文件滚动

设置很小的文件上限，确认：

- 新文件能够创建。
- 旧文件编号正确。
- 超出保留数量的文件被删除。
- 日志总量没有异常缺失。

### 23.11 启动失败

使用无权限路径或非法配置，确认 `start()` 明确失败，不会留下后台线程。

### 23.12 ThreadSanitizer

在 Linux 环境下使用：

```bash
-fsanitize=thread -g
```

运行并发测试，确保没有数据竞争报告。

---

## 24. CMake 要求

建议最低版本：

```cmake
cmake_minimum_required(VERSION 3.20)
```

项目要求：

```text
C++17
禁止编译器扩展
开启常见警告
链接线程库
```

核心配置：

```cmake
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
target_link_libraries(async_logger PRIVATE Threads::Threads)
```

GCC/Clang 建议：

```cmake
-Wall
-Wextra
-Wpedantic
-Wconversion
-Wshadow
```

---

## 25. 完成标准

当以下条件全部满足时，可以认为第一版异步日志库实现完成：

- [ ] 多线程可以安全调用 `log()`。
- [ ] 日志由独立后台线程写入文件。
- [ ] 队列有明确容量上限。
- [ ] 队列满时支持阻塞或丢弃策略。
- [ ] 支持日志级别过滤。
- [ ] 支持完整日志格式。
- [ ] 支持周期刷新。
- [ ] `flush()` 具有明确同步语义。
- [ ] `stop()` 幂等且不会死锁。
- [ ] 析构时不会丢失已经接收的日志。
- [ ] 支持按大小滚动文件。
- [ ] 后台线程异常不会触发 `std::terminate()`。
- [ ] 可以查询接收、写入和丢弃数量。
- [ ] 通过基础功能测试和多线程压力测试。
- [ ] ThreadSanitizer 未发现数据竞争。

---

## 26. 实现时最容易出错的地方

### 错误一：收到停止请求就直接退出

错误退出条件：

```text
state != Running
```

正确退出条件：

```text
state == Stopping && queue.empty()
```

---

### 错误二：持锁执行文件写入

文件 I/O 可能很慢。持锁写文件会让所有生产者无法提交日志。

正确方式：

```text
持锁取出一条日志
    ↓
释放锁
    ↓
格式化并写文件
```

---

### 错误三：持锁 `join()`

工作线程退出时通常还需要重新获取同一个锁。

如果停止线程持锁等待工作线程退出，就会产生死锁。

---

### 错误四：阻塞生产者无法在停止时退出

`stop()` 不仅要通知日志线程，还必须：

```cpp
space_cv_.notify_all();
```

否则阻塞在“等待队列空位”的业务线程可能永远无法醒来。

---

### 错误五：`flush()` 只调用 `ofstream::flush()`

如果队列中仍有日志，那么只刷新文件流并不能保证之前提交的日志已经写入。

`flush()` 必须先等待目标日志被后台线程消费，再刷新文件。

---

### 错误六：析构期间仍有线程调用 `log()`

日志对象析构前，调用方必须保证不再有业务线程访问它。

日志库可以保证自身析构正确，但无法修复外部悬空引用。

推荐生命周期顺序：

```text
停止业务线程
    ↓
等待业务线程退出
    ↓
停止日志库
    ↓
销毁日志对象
```

---

## 27. 第一版的设计定位

这一版本质上是一个：

```text
有界 MPSC 队列
+
单消费者后台线程
+
可靠停止协议
+
文件输出与滚动
```

它的核心训练价值不只是“把字符串写入文件”，而是练习：

- 多生产者与单消费者模型。
- 条件变量的正确使用。
- 有界队列和背压。
- 服务停止时的数据排空。
- 并发对象的生命周期管理。
- 后台线程异常处理。
- 文件资源的 RAII 管理。
- 明确、可测试的并发语义。

完成这一版后，再考虑双缓冲、线程局部缓存、固定缓冲区和批量系统调用等优化，才不会在复杂优化中掩盖基础并发错误。
