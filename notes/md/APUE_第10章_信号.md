# Unix 信号：现代服务端工程学习笔记

## 1. 信号到底是什么

信号是内核向进程或线程发送的一种**异步事件通知**。

常见场景：

| 信号      | 常见来源                     | 工程含义             |
| --------- | ---------------------------- | -------------------- |
| `SIGINT`  | 终端按下 `Ctrl+C`            | 请求程序停止         |
| `SIGTERM` | `kill`、systemd、容器平台    | 请求优雅退出         |
| `SIGHUP`  | 终端断开或人工发送           | 常用于重新加载配置   |
| `SIGCHLD` | 子进程退出或状态变化         | 通知父进程回收子进程 |
| `SIGPIPE` | 向已关闭的管道/socket 写数据 | 对端已经关闭连接     |
| `SIGSEGV` | 非法内存访问                 | 程序发生严重错误     |
| `SIGABRT` | `abort()`                    | 主动异常终止         |

信号不应该被理解成普通函数调用。它更像是：

```text
正常业务代码正在执行
        ↓
内核临时中断当前执行流
        ↓
执行信号处理函数
        ↓
处理函数返回
        ↓
恢复原来的执行位置
```

核心难点在于：

> 信号可能在程序执行到任意位置时到达。

因此，信号处理与多线程并发具有相似的风险，而且约束更加严格。

------

## 2. 信号的四个核心状态

理解信号机制，首先要区分四个概念。

### 2.1 产生：Generated

某个事件触发了信号，例如：

```cpp
::kill(pid, SIGTERM);
```

或者：

```text
用户按下 Ctrl+C
子进程退出
定时器到期
发生非法内存访问
```

此时称信号已经产生。

------

### 2.2 阻塞：Blocked

阻塞信号不是删除信号，而是告诉内核：

> 这个信号暂时不要递送给当前线程。

信号仍然可能产生，只是处理函数暂时不会运行。

------

### 2.3 未决：Pending

如果一个信号已经产生，但由于当前被阻塞而没有递送，它就处于未决状态。

```text
SIGTERM 产生
    ↓
SIGTERM 当前被阻塞
    ↓
SIGTERM 进入 pending 状态
```

传统信号通常不会精确记录到达次数。

例如阻塞期间连续收到三次 `SIGTERM`，解除阻塞后不一定执行三次处理函数。因此：

> 普通信号适合表达“某件事发生了”，不适合用来精确计数或传输任务。

------

### 2.4 递送：Delivered

当内核真正执行信号动作时，称为递送。

信号递送后可能：

- 执行用户注册的处理函数；
- 执行默认动作；
- 被忽略；
- 终止进程；
- 暂停或恢复进程。

完整过程：

```text
事件发生
   ↓
信号产生
   ↓
当前是否阻塞？
   ├── 是：进入 pending
   │          ↓
   │       解除阻塞
   │
   └── 否
         ↓
       信号递送
         ↓
处理函数 / 默认动作 / 忽略
```

------

## 3. 使用 `sigaction` 注册信号行为

现代 Unix/Linux 程序应使用 `sigaction()` 注册信号处理逻辑。

```cpp
#include <csignal>
#include <cstring>
#include <unistd.h>

volatile sig_atomic_t g_stop_requested = 0;

void handle_signal(int signo) {
    g_stop_requested = 1;
}

int main() {
    struct sigaction action {};
    action.sa_handler = handle_signal;

    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;

    if (::sigaction(SIGTERM, &action, nullptr) == -1) {
        return 1;
    }

    if (::sigaction(SIGINT, &action, nullptr) == -1) {
        return 1;
    }

    while (!g_stop_requested) {
        // 执行业务循环
    }

    // 正常执行资源释放和优雅退出
}
```

`sigaction` 中最重要的三个部分：

```cpp
struct sigaction {
    void (*sa_handler)(int); // 信号处理函数
    sigset_t sa_mask;        // handler 运行时额外屏蔽的信号
    int sa_flags;            // 行为配置
};
```

### `sa_mask`

处理函数运行期间，内核会临时屏蔽 `sa_mask` 中的信号。

这可以避免某些处理函数并发执行或嵌套执行。

### `SA_RESTART`

启用后，部分被信号打断的阻塞系统调用可以自动重新开始。

但是它不是万能的，代码仍然要意识到系统调用可能返回：

```cpp
errno == EINTR
```

------

## 4. 信号处理函数中的安全限制

信号处理函数运行在异常的异步上下文中。

假设主程序正在执行：

```cpp
std::vector<int> values;
values.push_back(10);
```

执行到一半时信号到达，处理函数也修改 `values`：

```cpp
void handler(int) {
    values.clear();
}
```

此时容器内部状态可能正在修改，程序行为不可预测。

更危险的情况是锁重入：

```text
主程序调用日志库
    ↓
日志库持有内部互斥锁
    ↓
信号到达
    ↓
handler 再次调用日志库
    ↓
尝试获取同一把锁
    ↓
死锁
```

因此，信号处理函数中不能随便调用普通函数。

### 不应该在 handler 中执行

```cpp
printf();
std::cout << "...";
new / delete;
malloc / free;
std::string 操作;
STL 容器操作;
pthread_mutex_lock();
普通日志库;
数据库操作;
复杂退出逻辑;
```

这些操作可能：

- 使用内部锁；
- 分配动态内存；
- 修改全局状态；
- 不具备异步信号安全性。

------

## 5. handler 的正确职责

信号处理函数应该尽可能短，只负责把信号转换成一个普通事件。

推荐做法有三种。

### 5.1 设置简单标志

```cpp
volatile sig_atomic_t g_stop_requested = 0;

void handle_sigterm(int) {
    g_stop_requested = 1;
}
```

业务线程在正常上下文中执行真正的退出逻辑：

```cpp
while (!g_stop_requested) {
    process_events();
}

stop_accepting_requests();
wait_for_running_tasks();
flush_logs();
close_connections();
```

注意：

```cpp
volatile sig_atomic_t
```

适合处理简单信号标志，但不应拿它实现复杂的线程同步。

------

### 5.2 Self-pipe 模式

创建一个管道：

```text
signal handler
      ↓
向 pipe 写入一个字节
      ↓
epoll 监听到 pipe 可读
      ↓
事件循环执行正常业务逻辑
```

处理函数只执行异步信号安全的 `write()`：

```cpp
int signal_pipe[2];

void handler(int signo) {
    const unsigned char value =
        static_cast<unsigned char>(signo);

    const auto unused =
        ::write(signal_pipe[1], &value, sizeof(value));

    (void)unused;
}
```

这样可以把信号整合进 Reactor 事件循环。

------

### 5.3 Linux `signalfd`

Linux 可以将信号转换为文件描述符事件：

```text
SIGTERM / SIGINT
       ↓
signalfd
       ↓
文件描述符可读
       ↓
epoll_wait 返回
       ↓
正常代码读取并处理信号
```

它的优势是可以统一监听：

```text
socket
eventfd
timerfd
signalfd
```

对于基于 `epoll` 的 Linux 服务端，`signalfd` 通常比异步 handler 更容易维护。

------

## 6. 信号集与信号屏蔽字

Unix 使用 `sigset_t` 表示一组信号。

```cpp
sigset_t set;

sigemptyset(&set);
sigaddset(&set, SIGTERM);
sigaddset(&set, SIGINT);
```

常用操作：

| 函数          | 作用                 |
| ------------- | -------------------- |
| `sigemptyset` | 清空信号集           |
| `sigfillset`  | 加入所有信号         |
| `sigaddset`   | 添加一个信号         |
| `sigdelset`   | 删除一个信号         |
| `sigismember` | 判断信号是否在集合中 |

不要假设 `sigset_t` 的底层类型，也不要直接对它进行位运算。

------

## 7. 使用 `sigprocmask` 控制信号递送

单线程程序可以使用 `sigprocmask()` 修改信号屏蔽字。

```cpp
sigset_t block_set;
sigset_t old_set;

sigemptyset(&block_set);
sigaddset(&block_set, SIGTERM);

if (::sigprocmask(
        SIG_BLOCK,
        &block_set,
        &old_set) == -1) {
    // 处理错误
}
```

三种操作：

| 参数          | 含义                 |
| ------------- | -------------------- |
| `SIG_BLOCK`   | 在原屏蔽字中增加信号 |
| `SIG_UNBLOCK` | 从原屏蔽字中解除信号 |
| `SIG_SETMASK` | 直接替换当前屏蔽字   |

恢复原来的屏蔽字：

```cpp
::sigprocmask(SIG_SETMASK, &old_set, nullptr);
```

`SIGKILL` 和 `SIGSTOP` 无法被捕获、忽略或阻塞。

------

## 8. 多线程程序中的信号屏蔽

在多线程程序里，信号屏蔽字是**线程级属性**。

因此，多线程程序应使用：

```cpp
pthread_sigmask()
```

常见工程模式：

```text
主线程启动
    ↓
主线程屏蔽 SIGINT、SIGTERM、SIGHUP
    ↓
创建业务线程
    ↓
子线程继承主线程的信号屏蔽字
    ↓
创建专用信号线程
    ↓
信号线程使用 sigwait 同步等待信号
```

示例：

```cpp
#include <pthread.h>
#include <signal.h>
#include <thread>

int main() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);

    if (::pthread_sigmask(SIG_BLOCK, &set, nullptr) != 0) {
        return 1;
    }

    std::thread signal_thread([set]() mutable {
        int signo = 0;

        while (::sigwait(&set, &signo) == 0) {
            if (signo == SIGINT || signo == SIGTERM) {
                // 通知业务系统开始退出
                break;
            }
        }
    });

    // 创建业务线程、启动服务器

    signal_thread.join();
}
```

### 为什么推荐 `sigwait`

`sigwait()` 是同步等待，不是在异步 handler 中执行。

这意味着接收到信号后，可以在普通线程上下文中：

- 获取锁；
- 写日志；
- 修改 C++ 对象；
- 通知条件变量；
- 调用正常的退出逻辑。

因此，对于现代多线程 C++ 服务端：

> 屏蔽信号并由专用线程使用 `sigwait()`，通常比异步 handler 更稳妥。

------

## 9. 使用 `sigsuspend` 无竞态地等待信号

下面的代码存在竞态：

```cpp
if (!signal_received) {
    pause();
}
```

可能发生：

```text
检查 signal_received，结果为 false
        ↓
信号到达，handler 设置为 true
        ↓
handler 返回
        ↓
程序调用 pause
        ↓
由于信号已经处理完，程序永久等待
```

根本问题是：

> 解除屏蔽和进入睡眠不是原子操作。

`sigsuspend()` 可以原子地完成：

```text
临时替换信号屏蔽字
        +
进入睡眠等待信号
```

典型结构：

```cpp
sigset_t block_set;
sigset_t old_set;

sigemptyset(&block_set);
sigaddset(&block_set, SIGUSR1);

sigprocmask(SIG_BLOCK, &block_set, &old_set);

while (!signal_received) {
    sigsuspend(&old_set);
}

sigprocmask(SIG_SETMASK, &old_set, nullptr);
```

学习时需要理解它解决的竞态问题，但在现代多线程服务端中，通常更常使用：

```text
sigwait
signalfd
eventfd
条件变量
事件循环
```

------

## 10. 系统调用被信号中断：`EINTR`

线程阻塞在系统调用中：

```cpp
ssize_t n = ::read(fd, buffer, size);
```

此时信号到达，系统调用可能返回：

```cpp
n == -1
errno == EINTR
```

可靠调用通常需要考虑重试：

```cpp
ssize_t retry_read(
    int fd,
    void* buffer,
    std::size_t size) {

    while (true) {
        const ssize_t result = ::read(fd, buffer, size);

        if (result >= 0) {
            return result;
        }

        if (errno != EINTR) {
            return -1;
        }
    }
}
```

但并不是所有 `EINTR` 都应该无脑重试。

例如：

- 应用正在退出；
- 调用本身带超时；
- 信号用于主动打断阻塞；
- 重试后可能超过业务 deadline。

工程判断应该是：

```text
errno == EINTR
    ↓
业务是否仍然允许继续等待？
    ├── 是：重新调用
    └── 否：返回上层处理
```

### `SA_RESTART` 的边界

设置：

```cpp
action.sa_flags = SA_RESTART;
```

可以让部分阻塞系统调用自动重启。

但仍然要注意：

- 不是所有系统调用都支持自动重启；
- 不同接口语义可能不同；
- 超时类调用需要重新计算剩余时间；
- 退出流程可能不希望继续阻塞。

所以应把 `SA_RESTART` 看成辅助机制，而不是完整解决方案。

------

## 11. `kill`：向进程发送信号

```cpp
int kill(pid_t pid, int signo);
```

常见用法：

```cpp
::kill(pid, SIGTERM);
```

表示请求目标进程退出。

```cpp
::kill(pid, SIGKILL);
```

表示由内核强制终止目标进程，目标进程无法清理资源。

### `SIGTERM` 与 `SIGKILL`

```text
SIGTERM：
通知进程主动退出
允许执行清理逻辑
适合正常停止服务

SIGKILL：
内核立即终止进程
无法捕获、无法阻塞
不会执行用户态清理逻辑
适合进程已经失控
```

生产环境通常先发送 `SIGTERM`，等待一段时间后仍未退出，再升级为 `SIGKILL`。

这也是 systemd、Docker、Kubernetes 常见的停止流程。

### `kill(pid, 0)`

```cpp
::kill(pid, 0);
```

不会真正发送信号，常用于检查：

- 目标进程是否存在；
- 当前进程是否有权限向其发送信号。

但要注意 PID 可能复用，因此不能只依赖 PID 判断某个具体服务实例是否仍然存活。

------

## 12. `SIGCHLD` 与子进程回收

子进程退出后，内核会向父进程发送 `SIGCHLD`。

父进程需要使用 `waitpid()` 回收子进程，否则会产生僵尸进程。

```cpp
void handle_sigchld(int) {
    const int saved_errno = errno;

    while (::waitpid(-1, nullptr, WNOHANG) > 0) {
    }

    errno = saved_errno;
}
```

为什么要循环调用？

因为普通信号不会保证一一排队：

```text
子进程 A 退出
子进程 B 退出
子进程 C 退出
        ↓
父进程可能只观察到一次 SIGCHLD
```

因此收到一次通知后，要循环回收所有已经退出的子进程：

```cpp
while (::waitpid(-1, nullptr, WNOHANG) > 0) {
}
```

不过在复杂 C++ 服务端中，更推荐：

- 专门的子进程管理线程；
- 使用 `signalfd` 统一处理；
- 在正常上下文中循环调用 `waitpid`；
- 明确记录子进程 PID 与业务任务的对应关系。

------

## 13. `SIGPIPE`：网络服务必须关注的信号

当程序向已经关闭的管道或 socket 写数据时，可能收到：

```text
SIGPIPE
```

默认动作通常是终止进程。

这对于网络服务器非常危险：

```text
客户端断开连接
    ↓
服务端继续 write/send
    ↓
产生 SIGPIPE
    ↓
整个服务进程被终止
```

常见处理方式：

### 方式一：进程级忽略 `SIGPIPE`

```cpp
struct sigaction action {};
action.sa_handler = SIG_IGN;

sigemptyset(&action.sa_mask);
action.sa_flags = 0;

::sigaction(SIGPIPE, &action, nullptr);
```

之后写失败会通过返回值体现：

```cpp
errno == EPIPE
```

### 方式二：使用发送选项

Linux 的 `send()` 可以使用：

```cpp
MSG_NOSIGNAL
ssize_t n = ::send(
    fd,
    data,
    size,
    MSG_NOSIGNAL);
```

服务端必须检查写操作结果：

```cpp
if (n == -1 && errno == EPIPE) {
    // 对端已经关闭
    // 关闭连接并清理连接状态
}
```

现代网络库通常会替应用处理这类问题，但理解 `SIGPIPE` 仍然十分重要。

------

## 14. 优雅退出的标准流程

信号在服务端最典型的用途，是实现优雅退出。

收到 `SIGTERM` 后，不应该立即在 handler 中释放所有资源。

推荐流程：

```text
收到 SIGTERM
      ↓
设置停止状态
      ↓
停止接受新连接
      ↓
停止接收新任务
      ↓
等待正在执行的请求完成
      ↓
设置最大退出期限
      ↓
刷新必要数据和日志
      ↓
关闭连接、线程池、文件描述符
      ↓
正常退出
```

伪代码：

```cpp
void Server::request_shutdown() {
    if (stopping_.exchange(true)) {
        return;
    }

    acceptor_.stop();
    worker_pool_.stop_accepting();
}

void Server::shutdown() {
    request_shutdown();

    worker_pool_.wait_for_completion(shutdown_deadline_);

    close_connections();
    flush_critical_data();
}
```

需要注意：

- 退出操作必须幂等；
- 不要无限等待业务任务；
- 应设置明确的 shutdown deadline；
- 达到期限后要取消或中断剩余任务；
- 不要在异步 handler 中执行这些复杂逻辑。

------

## 15. 配置重载与 `SIGHUP`

服务端有时使用 `SIGHUP` 触发配置重载：

```text
收到 SIGHUP
    ↓
通知配置线程
    ↓
读取新配置文件
    ↓
校验配置合法性
    ↓
构造新配置对象
    ↓
原子替换旧配置
```

不要在 handler 中直接读取配置文件。

推荐把信号转换成普通事件，然后在正常执行上下文中处理。

配置重载还应考虑：

- 新配置校验失败时继续使用旧配置；
- 配置对象应支持不可变快照；
- 更新过程应避免长时间持锁；
- 不是所有配置都适合在线热更新。

------

## 16. 崩溃信号应该怎样处理

常见崩溃信号包括：

```text
SIGSEGV
SIGABRT
SIGBUS
SIGFPE
SIGILL
```

收到这些信号时，进程状态可能已经损坏。

因此不要试图在 handler 中执行：

- 复杂日志格式化；
- 动态内存分配；
- 正常业务恢复；
- 获取普通互斥锁；
- 继续处理请求。

更合理的目标是：

```text
记录最小必要信息
      ↓
保留 core dump
      ↓
立即终止
      ↓
由外部进程管理器重启
```

生产环境应重点配置：

- core dump；
- 符号文件；
- 崩溃版本号；
- 构建 ID；
- systemd 或容器重启策略；
- `gdb`、`coredumpctl` 等诊断工具。

信号 handler 不是可靠的异常恢复机制。

------

## 17. C++ 服务端推荐架构

### 单线程 Reactor

推荐：

```text
阻塞目标信号
    ↓
创建 signalfd
    ↓
加入 epoll
    ↓
事件循环读取信号
    ↓
转换为 shutdown/reload 事件
```

架构示意：

```text
                    ┌──────── socket fd
                    │
epoll_wait ─────────┼──────── timerfd
                    │
                    ├──────── eventfd
                    │
                    └──────── signalfd
```

这样信号与网络事件采用统一的处理模型。

------

### 多线程服务器

推荐：

```text
主线程在创建其他线程前屏蔽信号
          ↓
所有业务线程继承屏蔽字
          ↓
专用信号线程使用 sigwait
          ↓
信号线程通知 Server 对象
          ↓
正常执行优雅退出或配置重载
```

这样可以避免异步 handler 对 C++ 代码带来的大量限制。

------

## 18. 常见错误

### 错误一：在 handler 中写日志

```cpp
void handler(int) {
    logger.info("received signal");
}
```

日志库可能分配内存或持有锁，存在死锁风险。

------

### 错误二：在 handler 中直接停止整个服务器

```cpp
void handler(int) {
    server.stop();
}
```

`stop()` 内部很可能访问复杂对象、锁和容器。

正确做法是只发出通知。

------

### 错误三：使用普通 `bool`

```cpp
bool stopped = false;

void handler(int) {
    stopped = true;
}
```

异步信号处理场景下不应依赖普通变量进行通信。

简单程序可以使用：

```cpp
volatile sig_atomic_t
```

复杂多线程程序应使用 `sigwait`、`signalfd`、管道或 `eventfd`。

------

### 错误四：认为 `SA_RESTART` 会处理所有 `EINTR`

即使设置了 `SA_RESTART`，仍然需要检查系统调用文档和业务语义。

------

### 错误五：用信号传输业务数据

普通信号可能合并，也不能承载复杂内容。

信号应只负责通知：

```text
退出
重载
子进程变化
发生某类事件
```

业务数据应通过：

- pipe；
- Unix domain socket；
- 消息队列；
- 共享内存；
- 网络协议。

------

### 错误六：用 `SIGKILL` 正常停止服务

`SIGKILL` 不会给程序清理资源的机会。

正常流程应优先发送：

```text
SIGTERM
```

只有服务无法正常退出时才使用 `SIGKILL`。

------

## 19. 本章学习重点

### 必须掌握

1. 信号是异步事件通知，不是普通函数调用。
2. 区分产生、阻塞、未决和递送。
3. 使用 `sigaction()` 注册信号行为。
4. handler 中只能执行极少量安全操作。
5. 理解 `EINTR` 和 `SA_RESTART`。
6. 使用信号屏蔽字控制递送时机。
7. 理解 `sigsuspend()` 解决的等待竞态。
8. 掌握 `SIGCHLD + waitpid(WNOHANG)`。
9. 掌握网络程序中的 `SIGPIPE`。
10. 掌握 `SIGTERM` 优雅退出流程。

### 服务端工程重点

1. 多线程程序优先使用 `pthread_sigmask + sigwait`。
2. Reactor 程序可以使用 `signalfd + epoll`。
3. handler 只负责通知，不负责执行复杂业务。
4. 信号适合作为控制面事件，不适合作为业务消息。
5. 崩溃后优先保留 core dump，而不是尝试继续运行。

------

## 20. 最终知识结构

```text
Unix 信号
│
├── 基础模型
│   ├── generated
│   ├── blocked
│   ├── pending
│   └── delivered
│
├── 信号配置
│   ├── sigaction
│   ├── sigset_t
│   ├── sigprocmask
│   └── pthread_sigmask
│
├── 安全问题
│   ├── 异步信号安全
│   ├── 函数重入
│   ├── EINTR
│   └── 竞态条件
│
├── 等待与接收
│   ├── sigsuspend
│   ├── sigwait
│   └── signalfd
│
├── 服务端场景
│   ├── SIGTERM 优雅退出
│   ├── SIGHUP 配置重载
│   ├── SIGCHLD 子进程回收
│   └── SIGPIPE 连接断开
│
└── 工程原则
    ├── handler 只做最小通知
    ├── 复杂逻辑回到正常上下文
    ├── 不使用信号传输业务数据
    └── 崩溃依靠 core dump 排查
```

------

## 总结

信号这一章真正要学习的不是大量 API，而是理解一种特殊的异步并发模型：

> 信号可以在任意时刻打断程序，因此处理函数必须受到严格限制；现代程序通过 `sigaction`、信号屏蔽字、`sigwait` 或 `signalfd`，把异步信号转换成可控的普通事件。

对于现代 C++ 服务端，可以进一步压缩成三条原则：

1. 使用 `sigaction`，不要依赖旧式信号语义。
2. handler 只通知，不执行复杂业务。
3. 多线程服务优先使用 `sigwait`，事件循环优先考虑 `signalfd`。