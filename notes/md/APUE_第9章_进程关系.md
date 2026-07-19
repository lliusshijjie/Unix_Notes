# 《UNIX 环境高级编程》第九章：进程关系（精华版）

> 本章不需要逐个 API 背诵。  
> 对 C++ 服务端工程师而言，真正值得掌握的是一条主线：

```text
进程
  ↓ 组织
进程组（Process Group）
  ↓ 进一步组织
会话（Session）
  ↓ 关联
控制终端（Controlling Terminal）
  ↓ 支撑
Shell 作业控制、前后台切换、终端信号
```

本章的价值，不在于日常业务代码直接使用，而在于帮助你理解：

- Shell 为什么能管理前台任务和后台任务；
- `Ctrl+C`、`Ctrl+Z` 为什么会影响一组进程；
- SSH 断开后，某些程序为什么退出；
- 守护进程为什么要脱离终端；
- 信号为什么常常不是发给单个进程，而是发给进程组。

---

## 1. 本章在讲什么

第八章关注的是：

> 一个进程如何创建、退出和回收另一个进程。

第九章进一步关注的是：

> 多个进程之间如何被组织，以及它们和终端、Shell 之间是什么关系。

一个 Shell 命令可能并不只对应一个进程。

例如：

```bash
cat access.log | grep ERROR | less
```

这条命令通常会产生多个进程：

```text
cat ──pipe──> grep ──pipe──> less
```

Shell 必须能够把它们视为一个整体：

- 一起放到前台运行；
- 一起接收 `Ctrl+C`；
- 一起暂停；
- 一起切换到后台。

这个“整体”就是**进程组**。

---

# 2. 进程组：把多个进程当成一个作业

## 2.1 基本概念

进程组是一组相关进程的集合。

每个进程都拥有：

```text
PID   ：进程 ID
PGID  ：进程组 ID
```

进程组中会有一个**进程组组长**：

```text
组长进程的 PID == 进程组 PGID
```

例如：

```text
Shell 创建管道任务：

PID=5001  cat
PID=5002  grep
PID=5003  less

三者的 PGID 都可能是 5001
```

结构如下：

```text
Process Group 5001
├── cat   PID=5001
├── grep  PID=5002
└── less  PID=5003
```

进程组最重要的意义是：

> 操作系统和 Shell 可以一次性管理一整组相关进程。

---

## 2.2 常用接口

```c
pid_t getpgrp(void);
pid_t getpgid(pid_t pid);

int setpgid(pid_t pid, pid_t pgid);
```

最常见的理解方式：

```c
setpgid(0, 0);
```

含义是：

```text
pid  = 0：操作当前进程
pgid = 0：使用当前进程 PID 作为进程组 ID
```

也就是让当前进程成为一个新进程组的组长。

---

## 2.3 为什么 Shell 需要进程组

执行下面的命令：

```bash
sleep 100 | cat
```

Shell 通常会：

1. `fork()` 创建多个子进程；
2. 将这些子进程放入同一个进程组；
3. 如果是前台任务，把该进程组设置为终端前台进程组；
4. 等待整个进程组运行结束或停止。

因此按下：

```text
Ctrl+C
```

终端驱动产生的 `SIGINT` 不是只发给某一个进程，而是发给：

> 当前终端的前台进程组。

这也是为什么管道中的多个程序通常会一起退出。

---

# 3. 会话：对多个进程组再做一层组织

## 3.1 基本结构

一个会话可以包含多个进程组。

```text
Session
├── 前台进程组
├── 后台进程组 A
└── 后台进程组 B
```

例如在一个 Shell 中：

```bash
sleep 100 &
vim test.cpp
```

可能形成：

```text
一个 Shell 会话
├── Shell 自己所在进程组
├── sleep 后台进程组
└── vim 前台进程组
```

会话的核心作用是：

> 描述一批属于同一个登录终端或 Shell 环境的进程。

---

## 3.2 创建会话：setsid

```c
pid_t setsid(void);
```

调用成功后，当前进程会：

1. 创建一个新会话；
2. 成为会话首进程；
3. 成为一个新进程组的组长；
4. 脱离原来的控制终端。

结构变化可以理解为：

```text
调用 setsid() 前：

旧 Session
└── 当前进程所在进程组
    └── 当前进程


调用 setsid() 后：

新 Session
└── 新 Process Group
    └── 当前进程
```

注意：

> 进程组组长不能直接调用 `setsid()` 创建新会话。

因此传统守护进程通常先 `fork()`：

```c
pid_t pid = fork();

if (pid > 0) {
    _exit(0);
}

if (setsid() == -1) {
    // error
}
```

子进程不是进程组组长，因此可以调用 `setsid()`。

---

# 4. 控制终端：进程与终端之间的纽带

## 4.1 什么是控制终端

一个会话可以拥有一个控制终端，例如：

```text
/dev/tty
/dev/pts/0
/dev/pts/1
```

本地终端、SSH 登录和终端模拟器通常都会创建伪终端。

可以把它理解为：

```text
用户键盘输入
    ↓
控制终端
    ↓
前台进程组
```

终端不仅负责输入输出，还会生成信号。

例如：

| 终端操作 | 常见信号 | 作用 |
|---|---:|---|
| `Ctrl+C` | `SIGINT` | 中断前台进程组 |
| `Ctrl+\` | `SIGQUIT` | 终止并可能生成 core |
| `Ctrl+Z` | `SIGTSTP` | 暂停前台进程组 |
| 终端断开 | `SIGHUP` | 通知相关进程终端已挂断 |

---

## 4.2 前台进程组与后台进程组

一个控制终端在同一时刻只有一个**前台进程组**。

```text
控制终端 /dev/pts/0
├── 前台进程组：vim
├── 后台进程组：sleep
└── 后台进程组：某个管道任务
```

只有前台进程组可以正常读取终端输入。

相关接口：

```c
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgrp);
```

Shell 在启动前台任务时，会大致执行：

```text
创建子进程
  ↓
建立进程组
  ↓
tcsetpgrp() 将终端前台控制权交给子进程组
  ↓
等待作业结束或停止
  ↓
重新把终端前台控制权交还给 Shell
```

---

# 5. 作业控制：Shell 如何实现前台与后台任务

作业控制是本章最重要的实际应用。

常见命令：

```bash
command &
jobs
fg
bg
```

它们的底层核心不是线程调度，而是：

```text
进程组 + 控制终端 + 信号
```

---

## 5.1 前台运行

```bash
vim test.cpp
```

Shell 会：

1. 创建 `vim` 子进程；
2. 为其建立进程组；
3. 将该进程组设为终端前台进程组；
4. Shell 自己等待；
5. `vim` 结束后，Shell 收回终端控制权。

---

## 5.2 后台运行

```bash
sleep 100 &
```

Shell 会创建进程，但不会把它所在的进程组设为前台进程组。

因此：

```text
sleep 可以继续运行
但不能随意读取终端输入
```

如果后台进程尝试读取控制终端，内核通常会向其进程组发送：

```text
SIGTTIN
```

默认动作是暂停该进程组。

后台进程写终端时，在特定终端配置下可能收到：

```text
SIGTTOU
```

---

## 5.3 Ctrl+Z、bg 与 fg 的关系

假设正在前台运行：

```bash
vim test.cpp
```

按下：

```text
Ctrl+Z
```

终端会向前台进程组发送：

```text
SIGTSTP
```

进程暂停后，Shell 重新获得终端控制权。

执行：

```bash
bg
```

Shell 向该进程组发送：

```text
SIGCONT
```

让它在后台继续运行。

执行：

```bash
fg
```

Shell 会：

1. 把该进程组设为终端前台进程组；
2. 发送 `SIGCONT`；
3. 等待该作业。

因此：

```text
fg/bg 的操作对象，本质上是进程组，而不是单个进程。
```

---

# 6. Shell 执行命令时的进程关系

理解一个典型管道即可，不必记忆书中所有 Shell 差异。

```bash
ps aux | grep nginx
```

大致过程：

```text
Shell
  │
  ├── fork → ps
  ├── fork → grep
  │
  ├── 创建 pipe
  ├── 将 ps、grep 放入同一进程组
  ├── 设置文件描述符重定向
  ├── exec 执行目标程序
  └── 等待整个前台作业
```

结构如下：

```text
Session
│
├── Shell Process Group
│   └── bash
│
└── Foreground Process Group
    ├── ps
    └── grep
```

这也是为什么：

```bash
Ctrl+C
```

可以同时结束管道中的多个程序。

---

# 7. 孤儿进程组：需要知道概念，不必深挖

孤儿进程组不是简单的“父进程退出后的子进程”。

它有更严格的定义：

> 某进程组中的所有进程，其父进程要么位于该进程组中，要么位于另一个会话中。

工程上只需理解它解决的问题。

假设：

1. 一个 Shell 启动前台作业；
2. 作业被 `Ctrl+Z` 暂停；
3. Shell 意外退出；
4. 被暂停的进程可能再也无人恢复。

为了避免这组进程永久处于停止状态，当进程组变成孤儿进程组，并且其中存在停止进程时，内核会向它们发送：

```text
SIGHUP
SIGCONT
```

含义分别是：

```text
SIGHUP  ：原控制环境已经消失
SIGCONT ：先让停止的进程恢复运行
```

程序随后可以处理 `SIGHUP`，或者直接结束。

---

# 8. SSH 断开后程序为什么可能退出

通过 SSH 登录时，系统会为登录会话建立伪终端。

```text
SSH Client
    ↓
sshd
    ↓
Shell
    ↓
前台/后台任务
```

当 SSH 连接断开时：

```text
伪终端关闭
  ↓
相关进程可能收到 SIGHUP
  ↓
程序退出
```

因此直接运行：

```bash
./server &
```

并不意味着 SSH 断开后服务一定继续运行。

常见解决方法包括：

```bash
nohup ./server > server.log 2>&1 &
```

或使用：

```text
systemd
supervisord
Docker / Kubernetes
tmux / screen
```

生产环境更推荐由服务管理器负责生命周期，而不是依赖 `nohup`。

---

# 9. 本章与守护进程的关系

后续学习守护进程时，经常会看到：

```text
fork
  ↓
setsid
  ↓
脱离控制终端
  ↓
重设工作目录和文件权限
  ↓
关闭或重定向文件描述符
```

其中 `setsid()` 的作用正来自本章：

```text
创建新会话
成为会话首进程
成为进程组组长
脱离原控制终端
```

不过现代 Linux 服务一般不建议自己完整实现传统 daemon 化流程。

更常见的方式是：

```text
服务保持前台运行
    ↓
systemd 负责：
- 启动
- 停止
- 重启
- 日志
- 权限
- 资源限制
- 崩溃拉起
```

因此本章要理解机制，但不必把传统 daemon 模板当成业务重点。

---

# 10. 对 C++ 服务端工程师的实际价值

## 10.1 直接编码频率较低

普通服务端业务代码很少主动调用：

```c
setpgid()
tcsetpgrp()
tcgetpgrp()
```

因为生产服务通常由：

```text
systemd
Docker
Kubernetes
进程管理器
```

负责生命周期管理。

---

## 10.2 排查问题时很有用

以下问题都和本章有关：

### 问题一：SSH 断开后程序退出

排查方向：

```text
是否绑定控制终端？
是否收到 SIGHUP？
是否使用 systemd/tmux/nohup？
```

### 问题二：Ctrl+C 为什么结束多个进程

原因：

```text
终端向前台进程组发送 SIGINT。
```

### 问题三：后台程序读取标准输入后被暂停

原因：

```text
后台进程组读取控制终端，收到 SIGTTIN。
```

### 问题四：程序被 Shell 启动后，信号传播行为异常

排查：

```text
PID、PPID、PGID、SID 是否符合预期？
进程是否处于正确的会话和进程组？
```

---

# 11. 常用排查命令

## 11.1 查看 PID、父进程、进程组和会话

```bash
ps -o pid,ppid,pgid,sid,tpgid,tty,stat,cmd -p <pid>
```

字段含义：

| 字段 | 含义 |
|---|---|
| `PID` | 进程 ID |
| `PPID` | 父进程 ID |
| `PGID` | 进程组 ID |
| `SID` | 会话 ID |
| `TPGID` | 当前终端前台进程组 ID |
| `TTY` | 控制终端 |
| `STAT` | 进程状态 |

---

## 11.2 查看进程树

```bash
pstree -ap
```

适合查看：

```text
systemd
 └─ sshd
     └─ bash
         └─ server
```

---

## 11.3 查看当前 Shell 的作业

```bash
jobs -l
```

---

## 11.4 向整个进程组发送信号

```bash
kill -- -<PGID>
```

例如：

```bash
kill -- -1234
```

这里负数表示向进程组发送信号，而不是向 PID 为 1234 的单个进程发送。

在代码中也类似：

```c
kill(-pgid, SIGTERM);
```

---

# 12. 最小代码示例：观察 PID、PGID 与 SID

```cpp
#include <unistd.h>

#include <iostream>

int main() {
    std::cout << "pid  = " << getpid()  << '\n';
    std::cout << "ppid = " << getppid() << '\n';
    std::cout << "pgid = " << getpgrp() << '\n';
    std::cout << "sid  = " << getsid(0) << '\n';

    return 0;
}
```

编译：

```bash
g++ -std=c++17 process_relation.cpp -o process_relation
```

运行：

```bash
./process_relation
```

也可以配合：

```bash
ps -o pid,ppid,pgid,sid,tpgid,tty,stat,cmd
```

观察不同进程之间的组织关系。

---

# 13. 本章学习优先级

## 必须理解

- 进程组是多个相关进程的组织单位；
- 会话包含多个进程组；
- 一个控制终端同一时刻只有一个前台进程组；
- 终端信号通常发送给前台进程组；
- Shell 的作业控制建立在进程组、终端和信号之上；
- `setsid()` 用于创建新会话并脱离原控制终端。

## 知道即可

- `setpgid()`、`tcgetpgrp()`、`tcsetpgrp()` 的用途；
- 后台进程读写终端时的 `SIGTTIN`、`SIGTTOU`；
- 孤儿进程组收到 `SIGHUP` 和 `SIGCONT` 的原因；
- SSH 断开与 `SIGHUP` 的关系。

## 可以略读

- 不同 Unix 系统登录流程的历史实现差异；
- 各种传统终端设备的细节；
- 不同 Shell 在作业控制实现上的细微差异；
- 内核内部关于会话和进程组的数据结构实现。

---

# 14. 一句话总结

> 第九章的核心不是让你在业务代码中频繁操作进程关系，而是让你理解：Shell 如何利用进程组、会话、控制终端和信号，把多个进程组织成可管理的前台或后台作业。

最终只需记住这张图：

```text
Session
│
├── Shell Process Group
│
├── Background Process Group
│
└── Foreground Process Group
          ↑
          │
    Controlling Terminal
          │
  Ctrl+C / Ctrl+Z / 输入
```
