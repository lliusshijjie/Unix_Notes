# 《UNIX 环境高级编程》第八章：进程控制（下）

> 本文总结《APUE》第八章在 `wait3`、`wait4` 之后的全部小节，即：
>
> - 8.9 竞争条件
> - 8.10 `exec` 函数
> - 8.11 更改用户 ID 和组 ID
> - 8.12 解释器文件
> - 8.13 `system` 函数
> - 8.14 进程会计
> - 8.15 用户标识
> - 8.16 进程调度
> - 8.17 进程时间
> - 8.18 总结
>
> 本部分的工程主线是：
>
> ```text
> fork 创建子进程
>       ↓
> 处理父子进程竞争与同步
>       ↓
> exec 用目标程序替换子进程
>       ↓
> 设置进程权限、环境和文件描述符
>       ↓
> 运行、调度、计时和审计
> ```

---

# 1. 后半章整体架构

前半章主要解决：

```text
如何创建子进程
如何等待子进程
如何回收子进程
```

后半章进一步解决：

```text
父子进程如何避免执行顺序竞争
子进程如何运行一个全新的程序
程序如何切换用户身份和权限
Shell 脚本为什么可以直接执行
如何通过 system 启动命令
系统如何统计进程资源使用
如何判断进程对应的登录用户
如何影响进程调度优先级
如何测量进程消耗的 CPU 时间
```

可以将本章完整闭环理解为：

```text
fork
  ↓
父子进程产生两条执行流
  ↓
同步或处理竞争条件
  ↓
子进程 exec 新程序
  ↓
程序以特定身份和优先级运行
  ↓
exit / 信号终止
  ↓
父进程 wait 回收
  ↓
系统记录资源使用信息
```

---

# 2. 竞争条件

## 2.1 什么是竞争条件

竞争条件（Race Condition）是指：

> 多个执行实体访问或修改共享状态时，程序结果依赖于不可预测的执行先后顺序。

在本章中，最直观的执行实体就是：

- 父进程；
- 子进程。

`fork()` 返回后，父子进程由内核独立调度：

```c
pid_t pid = fork();

if (pid == 0) {
    printf("child\n");
} else {
    printf("parent\n");
}
```

输出可能是：

```text
parent
child
```

也可能是：

```text
child
parent
```

因为 `fork()` 并不保证：

- 父进程先运行；
- 子进程先运行；
- 两者轮流运行；
- 某个进程在另一个进程前执行到某一行。

因此：

> 只要程序正确性依赖父子进程的运行顺序，就必须建立显式同步。

---

## 2.2 `sleep` 不是同步机制

错误做法：

```c
if (pid > 0) {
    sleep(1);
}
```

这种写法只是让子进程“更有机会”先执行，不能建立确定性。

在以下情况下仍可能失败：

- 系统负载很高；
- 子进程被长时间抢占；
- 父进程睡眠结束后，子进程仍未执行；
- 调试器或追踪工具改变调度行为。

所以：

```text
sleep = 延迟执行
同步原语 = 保证时序关系
```

二者不能混用。

---

## 2.3 `waitpid` 只能同步“子进程终止”

父进程调用：

```c
waitpid(child_pid, NULL, 0);
```

可以保证：

> `waitpid` 返回时，指定子进程已经终止。

但是它不能解决执行过程中的任意阶段同步。

例如：

```text
子进程：
步骤 A
步骤 B
步骤 C
退出

父进程希望在子进程完成步骤 B 后继续
```

`waitpid` 只能等到子进程退出，不能只等待步骤 B 完成。

这时需要：

- pipe；
- 信号；
- socketpair；
- 共享内存加同步原语；
- 信号量；
- eventfd。

---

## 2.4 书中的父子进程同步思想

书中通常会使用类似以下接口表达同步关系：

```c
TELL_WAIT();
TELL_PARENT(pid);
TELL_CHILD(pid);
WAIT_PARENT();
WAIT_CHILD();
```

这些不是标准系统调用，而是对信号或其他 IPC 机制的封装。

语义可以理解为：

```text
TELL_WAIT     初始化同步机制
TELL_PARENT   子进程通知父进程
TELL_CHILD    父进程通知子进程
WAIT_PARENT   子进程等待父进程
WAIT_CHILD    父进程等待子进程
```

例如要求父进程先执行：

```c
TELL_WAIT();

pid_t pid = fork();

if (pid == 0) {
    WAIT_PARENT();
    // 父进程通知后，子进程才继续
    child_work();
} else {
    parent_work();
    TELL_CHILD(pid);
}
```

时序变为：

```text
父进程                         子进程
  |                              |
parent_work                      WAIT_PARENT
  |                              |
TELL_CHILD --------------------> 唤醒
                                 |
                              child_work
```

这样程序不再依赖调度器“恰好”按照某种顺序运行。

---

## 2.5 使用管道建立父子同步

相比信号，管道更容易直观理解。

```c
#include <unistd.h>
#include <stdlib.h>

int main(void)
{
    int sync_pipe[2];

    if (pipe(sync_pipe) < 0) {
        _exit(1);
    }

    pid_t pid = fork();

    if (pid < 0) {
        _exit(1);
    }

    if (pid == 0) {
        char token;

        close(sync_pipe[1]);

        // 父进程写入前，子进程阻塞在这里
        if (read(sync_pipe[0], &token, 1) != 1) {
            _exit(1);
        }

        close(sync_pipe[0]);

        // 父进程已经完成准备
        child_work();
        _exit(0);
    }

    close(sync_pipe[0]);

    parent_prepare();

    // 通知子进程
    char token = 'X';
    write(sync_pipe[1], &token, 1);
    close(sync_pipe[1]);

    waitpid(pid, NULL, 0);
    return 0;
}
```

核心逻辑：

```text
子进程 read 管道
      ↓
管道中没有数据，因此阻塞
      ↓
父进程完成准备并 write
      ↓
子进程 read 返回并继续执行
```

这是一种真正的 happens-before 关系：

```text
父进程准备完成
    happens-before
子进程开始工作
```

---

## 2.6 竞争条件不仅发生在共享内存

父子进程普通变量相互独立，但仍然可能竞争：

- 同一个文件；
- 同一个 socket；
- 同一个 pipe；
- 同一个终端；
- 同一数据库记录；
- 同一外部设备；
- 同一目录中的文件名；
- 同一业务资源。

例如两个进程同时执行：

```c
if (access("job.lock", F_OK) != 0) {
    open("job.lock", O_CREAT | O_WRONLY, 0644);
}
```

这里存在典型的 TOCTOU 问题：

```text
检查文件不存在
      ↓
调度切换
      ↓
另一个进程创建文件
      ↓
当前进程继续创建
```

正确方式应使用一个原子操作完成检查与创建：

```c
int fd = open("job.lock",
              O_CREAT | O_EXCL | O_WRONLY,
              0644);
```

原则：

> 不要使用“先检查、后操作”的两个非原子步骤实现并发互斥。

---

## 2.7 服务端工程中的竞争条件

典型场景：

### 多 worker 竞争监听 socket

多个进程同时 `accept()`，可能产生：

- 惊群；
- 连接分配不均；
- 锁竞争。

现代系统可结合：

- pre-fork；
- `SO_REUSEPORT`；
- accept mutex；
- 单独 acceptor；
- 内核负载均衡。

### 多进程写日志

可能导致：

- 内容交叉；
- 文件偏移竞争；
- 日志轮转冲突。

常见方案：

- 单独日志进程；
- Unix domain socket；
- syslog/journald；
- 每 worker 独立文件；
- 单次原子写入并配合 `O_APPEND`。

### worker 重启

父进程需要防止：

- 同一个 PID 被重复回收；
- 同时启动过多 replacement worker；
- 崩溃循环导致 fork storm。

因此 master 进程通常维护明确的 worker 状态机。

---

# 3. `exec` 函数

## 3.1 `exec` 的核心作用

`fork()` 创建的是当前进程的副本，而 `exec` 用于：

> 使用一个新程序替换当前进程的程序映像。

```text
调用 exec 前：

进程 PID = 1001
程序 = 当前程序
代码段、数据段、堆、栈 = 当前程序内容

调用 exec 成功后：

进程 PID = 1001
程序 = 新程序
代码段、数据段、堆、栈 = 新程序内容
```

最关键的一点：

> `exec` 不创建新进程，进程 PID 通常保持不变。

它改变的是“进程正在运行的程序”。

因此经典模型是：

```text
fork：创建新进程
exec：让新进程执行目标程序
```

---

## 3.2 `exec` 成功不会返回

```c
execlp("ls", "ls", "-l", NULL);

printf("exec finished\n");
```

如果 `execlp` 成功：

```text
printf 永远不会执行
```

因为当前程序已经被 `ls` 替换。

只有失败时，`exec` 才返回 `-1`：

```c
execlp("ls", "ls", "-l", NULL);

perror("execlp");
_exit(127);
```

所以 `exec` 后面的代码必须按“失败处理路径”编写。

---

## 3.3 `exec` 函数族

常见接口包括：

```c
int execl(const char *path,
          const char *arg0, ...);

int execv(const char *path,
          char *const argv[]);

int execle(const char *path,
           const char *arg0, ...,
           char *const envp[]);

int execve(const char *path,
           char *const argv[],
           char *const envp[]);

int execlp(const char *file,
           const char *arg0, ...);

int execvp(const char *file,
           char *const argv[]);

int fexecve(int fd,
            char *const argv[],
            char *const envp[]);
```

理解这些名称，只需要拆解后缀。

---

## 3.4 `l` 与 `v`

### `l`：list

参数以可变参数列表传入：

```c
execl("/bin/ls",
      "ls",
      "-l",
      "/tmp",
      (char *)NULL);
```

适合参数数量固定、调用点明确的情况。

### `v`：vector

参数通过数组传入：

```c
char *argv[] = {
    "ls",
    "-l",
    "/tmp",
    NULL
};

execv("/bin/ls", argv);
```

适合动态构造参数。

可以记忆为：

```text
l = list，参数一个一个写
v = vector，参数放进 argv 数组
```

---

## 3.5 `p`：按 `PATH` 搜索

不带 `p`：

```c
execv("/bin/ls", argv);
```

必须提供明确路径。

带 `p`：

```c
execvp("ls", argv);
```

会根据环境变量 `PATH` 搜索可执行文件。

服务端程序使用 `p` 时需要谨慎：

- `PATH` 可能被修改；
- 可能执行到非预期程序；
- 高权限程序尤其不应依赖不可信 `PATH`。

生产代码更稳妥的方式是使用绝对路径：

```c
execv("/usr/bin/my-worker", argv);
```

---

## 3.6 `e`：显式提供环境变量

不带 `e` 的接口通常继承当前进程环境。

带 `e`：

```c
char *envp[] = {
    "PATH=/usr/bin:/bin",
    "LANG=C",
    NULL
};

execle("/usr/bin/env",
       "env",
       (char *)NULL,
       envp);
```

或者：

```c
execve(path, argv, envp);
```

适合需要构造干净运行环境的场景。

工程上建议：

- 不要把整个父进程环境无条件传给不可信子程序；
- 清理敏感变量；
- 明确设置 `PATH`、区域设置和运行模式；
- 注意代理、动态链接器和凭证相关环境变量。

---

## 3.7 `fexecve`：通过文件描述符执行

```c
int fd = open("/usr/bin/my-worker", O_RDONLY);

fexecve(fd, argv, envp);
```

它通过一个已经打开的文件描述符执行程序。

价值在于减少：

```text
检查路径
    ↓
路径被替换
    ↓
再次按照路径执行
```

这种路径级 TOCTOU 风险。

它适合：

- 已经安全打开目标文件；
- 希望执行精确的文件对象；
- 文件路径可能被重命名或替换。

---

## 3.8 `argv[0]` 是什么

执行：

```c
execl("/bin/ls",
      "my-ls",
      "-l",
      (char *)NULL);
```

新程序收到：

```text
argv[0] = "my-ls"
argv[1] = "-l"
```

内核通常不会强制要求 `argv[0]` 与可执行文件路径相同。

约定上：

```text
argv[0] = 程序名称
```

但它本质上由调用者传入，不能作为可靠安全身份。

---

## 3.9 `exec` 后哪些内容被替换

新程序会获得新的：

- 代码段；
- 初始化数据；
- 未初始化数据；
- 堆；
- 栈；
- 线程执行上下文。

原程序的普通变量、对象和函数不再存在。

例如：

```c
int value = 100;
execv(...);
```

`exec` 成功后，新程序不能继续访问原来的 `value`。

---

## 3.10 `exec` 后哪些进程属性通常保留

虽然程序映像被替换，但进程身份和部分内核状态仍然存在，例如：

- PID；
- 父进程 PID；
- 进程组 ID；
- 会话 ID；
- 当前工作目录；
- 根目录；
- umask；
- 资源限制；
- 大多数打开的文件描述符；
- 实际用户 ID 和实际组 ID。

这说明：

```text
exec 换的是程序映像
不是创建另一个进程身份
```

---

## 3.11 文件描述符继承与 `FD_CLOEXEC`

默认情况下，打开的文件描述符会跨 `exec` 保留。

例如父进程打开：

```c
int fd = open("data.txt", O_RDONLY);
```

子进程 `exec` 后，新程序仍可能拥有相同的 `fd`。

这既可以用于：

- 标准输入输出重定向；
- 向 worker 传递 socket；
- 传递 pipe；
- socket activation。

也可能造成严重问题：

- 子进程意外持有监听 socket；
- pipe 无法收到 EOF；
- 敏感文件泄露给外部程序；
- 文件或设备无法按预期关闭。

需要设置 close-on-exec：

```c
fcntl(fd, F_SETFD, FD_CLOEXEC);
```

创建时更推荐原子设置：

```c
open(path, O_RDONLY | O_CLOEXEC);
pipe2(pipefd, O_CLOEXEC);
socket(domain, type | SOCK_CLOEXEC, protocol);
```

为什么创建时设置更安全：

```text
open
  ↓
另一个线程 fork + exec
  ↓
fcntl 设置 CLOEXEC
```

在多线程程序中，上述两步之间存在竞争窗口。

因此：

> 能在创建描述符时设置 `CLOEXEC`，就不要事后再补。

---

## 3.12 信号处理在 `exec` 后的变化

一般而言：

- 被设置为默认动作的信号，保持默认；
- 被忽略的信号，通常继续忽略；
- 安装了自定义处理函数的信号，通常恢复为默认动作；
- 信号屏蔽字通常保留；
- 未决信号的具体语义需结合标准和平台理解。

原因是：

> 自定义信号处理函数属于旧程序代码，exec 后旧代码已经不存在。

因此新程序不能继续调用旧地址空间中的处理函数。

---

## 3.13 `fork + exec + waitpid` 完整范例

```c
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    fflush(NULL);

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        char *argv[] = {
            "ls",
            "-l",
            "/tmp",
            NULL
        };

        execv("/bin/ls", argv);

        // 只有 exec 失败才会运行到这里
        perror("execv");
        _exit(127);
    }

    int status;
    pid_t ret;

    do {
        ret = waitpid(pid, &status, 0);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        perror("waitpid");
        return 1;
    }

    if (WIFEXITED(status)) {
        printf("child exit code = %d\n",
               WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        printf("child killed by signal = %d\n",
               WTERMSIG(status));
    }

    return 0;
}
```

执行流程：

```text
父进程
  |
  | fork
  +-------------------------+
  |                         |
  v                         v
waitpid                  子进程
                            |
                     设置参数、环境、fd
                            |
                          exec
                            |
                       目标程序运行
                            |
                          exit
  |                         |
  +------ 获取状态 <--------+
```

---

## 3.14 在 C++ 多线程服务中使用 `exec`

多线程进程 `fork` 后，子进程只保留调用 `fork` 的线程，但会继承内存中的锁状态。

因此 fork 后到 exec 前不应：

- 使用 `std::cout`；
- 写复杂日志；
- 使用 `new/delete`；
- 获得 `std::mutex`；
- 操作复杂 STL 容器；
- 调用依赖后台线程的库。

工程原则：

```text
fork 后的子进程只做极少量、可控准备
然后立即 exec
```

更现代的场景可以考虑：

```text
posix_spawn
```

它把创建和执行过程封装得更适合复杂、多线程程序。

---

# 4. 更改用户 ID 和组 ID

## 4.1 三类用户 ID

Unix 进程通常需要理解三种用户身份：

### 实际用户 ID（real UID）

表示：

> 谁启动了这个进程。

### 有效用户 ID（effective UID）

内核进行大多数权限检查时使用的身份。

例如：

- 是否可以读取文件；
- 是否可以发送信号；
- 是否可以执行特权操作。

### 保存的设置用户 ID（saved set-user-ID）

用于保存某个权限身份，使程序可以在允许的情况下暂时放弃、之后恢复有效权限。

组 ID 也有类似概念：

- 实际组 ID；
- 有效组 ID；
- 保存的设置组 ID。

---

## 4.2 set-user-ID 程序

普通情况下：

```text
real UID      = 启动者
effective UID = 启动者
```

如果可执行文件设置了 set-user-ID 位，则执行时：

```text
effective UID = 文件所有者
```

例如某个文件由 root 所有并设置 set-user-ID：

```text
普通用户执行程序
real UID      = 普通用户
effective UID = root
```

这使程序可以执行有限的特权操作。

但 set-user-ID 程序风险极高：

- 输入验证错误可能变为提权漏洞；
- 环境变量可能影响执行；
- 路径搜索可能执行恶意程序；
- 文件描述符可能泄露；
- race condition 可能扩大权限边界。

---

## 4.3 `setuid`

```c
#include <unistd.h>

int setuid(uid_t uid);
```

其具体行为取决于调用进程当前权限。

简化理解：

### 特权进程调用

可以设置：

- 实际 UID；
- 有效 UID；
- 保存的设置 UID。

这通常意味着永久放弃原特权。

### 非特权进程调用

通常只能在以下身份范围内切换：

- 实际 UID；
- 保存的设置 UID。

不能随意变成任意用户。

---

## 4.4 临时降低权限与永久降低权限

一个服务可能需要：

1. 以 root 启动；
2. 绑定低端口或打开特权资源；
3. 切换为普通用户；
4. 运行绝大多数业务逻辑。

典型思路：

```text
高权限启动
   ↓
完成必要的特权初始化
   ↓
设置组身份
   ↓
设置用户身份
   ↓
永久放弃 root 权限
   ↓
执行普通业务
```

示意代码：

```c
if (setgid(target_gid) < 0) {
    perror("setgid");
    exit(1);
}

if (setuid(target_uid) < 0) {
    perror("setuid");
    exit(1);
}
```

顺序通常是：

```text
先 setgid
后 setuid
```

因为放弃 root 用户权限后，可能已经没有权限再修改组身份。

---

## 4.5 `seteuid` 和 `setegid`

```c
int seteuid(uid_t uid);
int setegid(gid_t gid);
```

它们用于修改有效用户或有效组身份。

某些特权程序会：

```text
需要特权时切换到高权限有效 UID
不需要时切回普通用户 UID
```

但这种“临时保留 root 并反复切换”的设计复杂且危险。

现代工程更推荐：

- 最小权限；
- 尽早永久降权；
- 将特权操作放到单独小进程；
- 使用 capabilities 等更细粒度权限；
- 不让大型业务模块长期持有 root 恢复能力。

---

## 4.6 权限切换的工程原则

### 先完成必要的资源初始化

例如：

- 绑定特权端口；
- 打开设备；
- 读取仅 root 可读配置；
- 创建运行目录；
- 设置资源限制。

### 然后永久降权

降权后应验证：

```c
if (geteuid() != target_uid) {
    // privilege drop failed
}
```

### 不要信任环境变量

高权限程序应特别警惕：

- `PATH`；
- `IFS`；
- 动态链接器相关变量；
- 语言运行时搜索路径；
- 临时目录；
- 配置文件路径。

### 不要通过 Shell 拼接命令

set-user-ID 程序调用：

```c
system(user_input);
```

通常是非常危险的设计。

---

## 4.7 服务端常见的权限拆分模型

```text
master 进程
高权限
  |
  | 完成监听端口、资源初始化
  v
fork worker
  |
  | worker setgid + setuid
  v
普通用户 worker
```

或者：

```text
业务进程（低权限）
       |
       | Unix domain socket
       v
特权辅助进程（仅暴露少量操作）
```

第二种模型的优势是：

- 特权代码体积小；
- 攻击面更可控；
- 权限边界明确；
- 业务进程被攻破后权限有限。

---

# 5. 解释器文件

## 5.1 什么是解释器文件

解释器文件通常以：

```text
#!
```

开头，例如：

```sh
#!/bin/sh
echo "hello"
```

或者：

```python
#!/usr/bin/python3
print("hello")
```

当该文件具有执行权限并被执行时，内核识别首行并启动指定解释器。

---

## 5.2 `#!` 行的基本格式

```text
#! pathname [optional-argument]
```

例如：

```text
#!/bin/sh
```

或：

```text
#!/usr/bin/env python3
```

内核并不是直接执行脚本文本，而是大致转换为：

```text
解释器 可选参数 脚本路径 原始命令行参数
```

例如执行：

```bash
./script.sh arg1 arg2
```

其效果可近似理解为：

```bash
/bin/sh ./script.sh arg1 arg2
```

---

## 5.3 解释器文件与解释器程序

需要区分：

### 解释器文件

例如：

```text
script.py
script.sh
```

它通过 `#!` 指定解释器。

### 解释器程序

例如：

```text
/bin/sh
/usr/bin/python3
```

它本身是可执行程序。

执行解释器文件时，最终仍然是：

```text
exec 解释器程序
```

然后由解释器读取脚本内容。

---

## 5.4 `/usr/bin/env` 的作用

常见写法：

```text
#!/usr/bin/env python3
```

它不是让内核直接搜索 Python，而是先执行：

```text
/usr/bin/env
```

然后由 `env` 根据当前 `PATH` 查找 `python3`。

优点：

- 适应不同系统上的解释器安装位置；
- 对虚拟环境友好。

风险：

- 依赖当前 `PATH`；
- 高权限程序不应使用不可信 `PATH`；
- 不同系统对可选参数解析存在兼容性差异。

生产脚本应根据部署环境选择：

```text
固定绝对解释器路径
或
通过受控 PATH 使用 /usr/bin/env
```

---

## 5.5 可选参数的可移植性

经典 shebang 形式通常只保证一个可选参数：

```text
#! interpreter optional-arg
```

不要假设以下写法在所有 Unix 上都按 Shell 分词：

```text
#!/usr/bin/env python3 -I -S
```

不同系统可能把后面的内容：

- 作为一个整体参数；
- 或按不同规则解析。

如果需要复杂启动参数，更可靠的方式是：

- 写一个包装脚本；
- 在脚本内部设置；
- 使用目标平台支持的 `env -S`，但注意可移植性。

---

## 5.6 工程注意事项

- 脚本必须具有执行权限；
- shebang 路径必须存在；
- 行尾格式应正确，Windows CRLF 可能导致解释器路径异常；
- 高权限执行时不要依赖不可信 PATH；
- 脚本路径和参数仍需要正确转义；
- 解释器文件最终仍受 `exec`、文件描述符继承和权限模型影响。

---

# 6. `system` 函数

## 6.1 函数原型

```c
#include <stdlib.h>

int system(const char *cmdstring);
```

基本用法：

```c
int status = system("ls -l /tmp");
```

它通常相当于：

```text
fork
  ↓
子进程执行 /bin/sh -c "ls -l /tmp"
  ↓
父进程等待 Shell 结束
```

因此 `system` 执行的不是一个简单可执行文件，而是：

```text
Shell 命令字符串
```

这意味着命令中可以包含：

- 管道；
- 重定向；
- 通配符；
- 变量替换；
- `&&`、`||`；
- 子命令；
- 多条命令。

---

## 6.2 `system(NULL)`

```c
int available = system(NULL);
```

用于检查系统是否提供可用的命令处理器。

返回非 0 通常表示存在命令处理环境。

在常规 Unix 系统上一般有 `/bin/sh`，但可移植程序仍可按接口语义判断。

---

## 6.3 返回值不是简单退出码

错误写法：

```c
int code = system("cmd");

if (code == 0) {
    // success
}
```

`system` 返回的是类似 `waitpid` 的编码状态，还可能包含创建子进程或启动 Shell 失败的信息。

正确解析：

```c
int status = system("command");

if (status == -1) {
    perror("system");
} else if (WIFEXITED(status)) {
    int code = WEXITSTATUS(status);
    printf("exit code = %d\n", code);
} else if (WIFSIGNALED(status)) {
    printf("killed by signal = %d\n",
           WTERMSIG(status));
}
```

注意：

```text
Shell 成功启动
不代表目标命令成功
```

例如目标命令不存在时，通常是 Shell 返回一个非零退出码。

---

## 6.4 命令注入风险

危险代码：

```c
char command[1024];
snprintf(command,
         sizeof(command),
         "grep %s data.txt",
         user_input);

system(command);
```

如果用户输入：

```text
keyword; rm -rf /tmp/data
```

Shell 会把它解释成多个命令。

因此：

> 不要把不可信输入拼接进 `system` 命令字符串。

更安全的方案：

```c
char *argv[] = {
    "grep",
    user_input,
    "data.txt",
    NULL
};

execv("/usr/bin/grep", argv);
```

这里参数直接传给程序，不经过 Shell 语法解析。

---

## 6.5 `system` 的信号语义

实现 `system` 时需要处理复杂信号问题，例如：

- 调用进程等待期间如何处理 `SIGINT`；
- 如何处理 `SIGQUIT`；
- 如何避免调用者的 `SIGCHLD` 逻辑干扰内部等待；
- 被信号中断后如何恢复状态。

这也是为什么不建议自行随意实现一个不完整的 `system` 替代品。

书中分析 `system`，目的之一是让读者理解：

> 一个看似简单的“执行命令并等待”，背后涉及 fork、exec、wait 和信号状态管理。

---

## 6.6 什么时候适合使用 `system`

适合：

- 简单工具；
- 受控环境；
- 命令字符串完全由程序固定；
- 确实需要 Shell 管道、重定向等能力。

不适合：

- 服务端高频请求路径；
- 参数包含用户输入；
- 高权限进程；
- 需要精确控制超时、fd、环境和资源限制；
- 对性能和可靠性要求高的任务执行器。

生产环境通常优先：

```text
posix_spawn
或
fork + exec
```

---

# 7. 进程会计

## 7.1 什么是进程会计

进程会计（Process Accounting）是 Unix 提供的一种系统级统计机制。

启用后，内核会在进程终止时记录该进程的部分信息，例如：

- 命令名称；
- 用户 ID；
- 组 ID；
- 启动和运行时间；
- 用户态 CPU 时间；
- 内核态 CPU 时间；
- 内存或 I/O 使用信息；
- 终止状态；
- 是否以特殊方式执行。

它主要用于：

- 系统使用统计；
- 资源审计；
- 用户计费；
- 离线分析；
- 追踪异常进程。

---

## 7.2 记录发生在进程终止时

一个重要特点：

> 会计记录通常在进程结束时写入。

因此它不是实时监控接口。

对于长时间运行的服务：

```text
服务运行几个月
只有退出时才形成完整记录
```

所以它不适合直接替代：

- 实时监控；
- Prometheus 指标；
- cgroup 资源统计；
- `/proc` 采样；
- tracing；
- profiler。

---

## 7.3 进程会计的局限

### 命令名称可能被截断

传统会计结构中的命令名字段长度有限，不能依赖它保存完整命令行。

### fork 与 exec 的归属可能难理解

一个进程：

```text
fork 出来
执行一段代码
exec 新程序
最终退出
```

最终会计记录描述的是同一个 PID 生命周期中的累计资源使用，但命令标识和执行阶段可能无法完整表达全部历史。

### 数据精度和字段依赖系统实现

不同 Unix/Linux 系统的格式和能力存在差异。

### 只能看到结果，难以解释原因

会计数据可以说明：

```text
消耗了多少 CPU
运行了多久
```

但不能直接说明：

```text
为什么慢
在哪个函数消耗
等待了哪个锁
```

---

## 7.4 现代服务端如何补充

现代生产环境更常使用：

- `/proc/<pid>`；
- cgroups；
- systemd unit 统计；
- `getrusage`；
- eBPF；
- perf；
- Prometheus；
- OpenTelemetry；
- 容器运行时指标。

进程会计的价值更多在于：

> 理解 Unix 如何在进程退出时保留资源使用记录，以及审计系统的基础思想。

---

# 8. 用户标识

## 8.1 `getlogin`

```c
#include <unistd.h>

char *getlogin(void);
```

它尝试返回与当前会话关联的登录用户名。

需要注意：

> 登录用户名不一定等于进程有效用户 ID 对应的用户名。

例如：

```text
用户 alice 登录
    ↓
使用 sudo 运行程序
    ↓
进程 effective UID 可能是 root
    ↓
登录用户仍可能是 alice
```

---

## 8.2 为什么 `getlogin` 可能失败

`getlogin()` 依赖登录会话相关信息。

以下场景可能没有合适的登录名：

- 守护进程；
- cron 任务；
- 容器进程；
- systemd 服务；
- 没有控制终端的进程；
- 登录数据库信息不完整。

因此服务端代码不能假设：

```c
getlogin() 永远成功
```

必须检查返回值。

---

## 8.3 根据 UID 查询用户名

如果目标是：

> 查询某个 UID 对应的账户名称

应使用用户数据库接口，例如：

```c
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

struct passwd *pw = getpwuid(getuid());

if (pw != NULL) {
    printf("user = %s\n", pw->pw_name);
}
```

区分：

```text
getlogin()
    询问当前登录会话是谁

getpwuid(getuid())
    查询实际 UID 对应的账户

getpwuid(geteuid())
    查询有效 UID 对应的账户
```

它们回答的不是同一个问题。

---

## 8.4 工程中的身份判断

根据目的选择：

### 权限判断

使用内核提供的 UID/GID：

```c
geteuid();
getegid();
```

### 展示账户名称

通过：

```c
getpwuid();
```

查询用户数据库。

### 审计原始登录者

需要结合：

- 登录会话；
- PAM；
- audit；
- systemd；
- SSH 连接信息；
- sudo 日志。

不要把以下环境变量直接视为可信安全身份：

```text
USER
LOGNAME
SUDO_USER
```

环境变量可被调用者修改，最多作为显示或辅助信息。

---

# 9. 进程调度

## 9.1 调度优先级的基本认识

操作系统调度器决定：

```text
哪个可运行进程
在哪个 CPU
运行多长时间
何时被抢占
```

普通进程通常不能直接命令内核：

```text
必须立即运行我
```

它只能通过优先级、调度策略和资源控制向内核表达倾向。

---

## 9.2 nice 值

Unix 使用 nice 值表示普通进程的调度倾向。

基本规律：

```text
nice 值越大
进程越“谦让”
调度优先级倾向越低
```

```text
nice 值越小
进程优先级倾向越高
```

在 Linux 上常见范围是：

```text
-20 到 19
```

但可移植程序不应过度依赖具体范围。

---

## 9.3 `nice`

```c
#include <unistd.h>

int nice(int incr);
```

它对当前 nice 值增加一个增量：

```c
nice(5);
```

表示让进程更谦让。

注意：

```text
nice 的返回值可能合法地为 -1
```

因此判断错误时需要结合 `errno`：

```c
errno = 0;
int value = nice(0);

if (value == -1 && errno != 0) {
    perror("nice");
}
```

---

## 9.4 `getpriority` 与 `setpriority`

```c
#include <sys/resource.h>

int getpriority(int which, id_t who);

int setpriority(int which,
                id_t who,
                int value);
```

可以操作：

- 单个进程；
- 进程组；
- 某个用户的进程集合。

例如：

```c
int current = getpriority(PRIO_PROCESS, 0);
```

其中 `who = 0` 通常表示当前调用者。

设置当前进程：

```c
setpriority(PRIO_PROCESS, 0, 10);
```

---

## 9.5 权限限制

普通用户通常可以：

```text
增大 nice 值
降低自己的调度优先级
```

但不能随意：

```text
减小 nice 值
提高调度优先级
```

提高优先级通常需要特权。

这符合资源安全原则：

> 普通进程可以主动少占 CPU，但不能随意抢占更多 CPU。

---

## 9.6 nice 不是实时保证

设置较高优先级不意味着：

- 一定立即运行；
- 一定比其他进程先完成；
- 不会被抢占；
- 延迟一定满足上限。

nice 只是普通调度策略中的权重或倾向。

对于严格实时需求，需要研究：

- `sched_setscheduler`；
- `SCHED_FIFO`；
- `SCHED_RR`；
- CPU affinity；
- 实时内核；
- 内存锁定；
- 中断和 CPU 隔离。

这些不应仅靠 nice 解决。

---

## 9.7 服务端工程中的使用方式

### 后台低优先级任务

例如：

- 日志压缩；
- 离线索引构建；
- 缓存预热；
- 低优先级数据扫描。

可以适当提高 nice 值，减少对在线请求的影响。

### 在线请求进程

不要简单通过降低 nice 值解决延迟问题。

应优先分析：

- CPU 是否饱和；
- 是否存在锁竞争；
- 是否有长尾任务；
- 线程数量是否合理；
- 是否存在 NUMA 或 CPU 迁移问题；
- 是否需要 cgroup CPU 配额和权重。

### 容器环境

现代服务常通过：

- cgroup CPU weight；
- CPU quota；
- cpuset；
- Kubernetes requests/limits；

实现整体资源隔离，而不是只依赖进程 nice。

---

# 10. 进程时间

## 10.1 四类进程时间

分析一个程序时，需要区分：

### 墙上时钟时间（wall-clock time）

从程序开始到结束，现实世界经过的时间。

包括：

- CPU 执行；
- I/O 等待；
- 锁等待；
- sleep；
- 被抢占；
- 调度延迟。

### 用户态 CPU 时间

CPU 在用户态执行该进程代码的时间。

例如：

- 算法计算；
- 用户态库代码；
- 数据处理。

### 内核态 CPU 时间

CPU 在内核态为该进程执行系统调用和内核工作的时间。

例如：

- `read/write`；
- page fault；
- 网络协议栈；
- 文件系统；
- 调度和内存管理相关工作。

### 子进程 CPU 时间

已经终止并被等待回收的子进程所消耗的用户态和内核态 CPU 时间。

---

## 10.2 `times`

```c
#include <sys/times.h>

clock_t times(struct tms *buf);
```

结构：

```c
struct tms {
    clock_t tms_utime;   // 当前进程用户态 CPU 时间
    clock_t tms_stime;   // 当前进程内核态 CPU 时间
    clock_t tms_cutime;  // 已等待子进程用户态 CPU 时间
    clock_t tms_cstime;  // 已等待子进程内核态 CPU 时间
};
```

调用：

```c
struct tms begin_tms;
struct tms end_tms;

clock_t begin = times(&begin_tms);

// 执行任务

clock_t end = times(&end_tms);
```

---

## 10.3 时钟滴答转换

`times()` 返回值和 `struct tms` 字段通常使用时钟滴答数。

获取每秒滴答数：

```c
long ticks_per_second = sysconf(_SC_CLK_TCK);
```

转换：

```c
double seconds =
    (double)(end - begin) / ticks_per_second;
```

不要写死：

```c
100 ticks per second
```

因为不同系统配置可能不同。

---

## 10.4 完整计时示例

```c
#include <stdio.h>
#include <sys/times.h>
#include <unistd.h>

int main(void)
{
    long ticks = sysconf(_SC_CLK_TCK);

    if (ticks <= 0) {
        return 1;
    }

    struct tms before;
    struct tms after;

    clock_t real_before = times(&before);

    if (real_before == (clock_t)-1) {
        return 1;
    }

    run_workload();

    clock_t real_after = times(&after);

    if (real_after == (clock_t)-1) {
        return 1;
    }

    printf("real  = %.3f s\n",
           (double)(real_after - real_before) / ticks);

    printf("user  = %.3f s\n",
           (double)(after.tms_utime - before.tms_utime) / ticks);

    printf("sys   = %.3f s\n",
           (double)(after.tms_stime - before.tms_stime) / ticks);

    printf("child user = %.3f s\n",
           (double)(after.tms_cutime - before.tms_cutime) / ticks);

    printf("child sys  = %.3f s\n",
           (double)(after.tms_cstime - before.tms_cstime) / ticks);

    return 0;
}
```

---

## 10.5 如何分析时间结果

### wall 很大，user/sys 很小

说明程序大部分时间可能在：

- 等待网络；
- 等待磁盘；
- sleep；
- 等锁；
- 等其他进程；
- 被调度器挂起。

### user 很大

说明主要消耗在用户态计算，例如：

- 编解码；
- 压缩；
- 排序；
- 序列化；
- 模型推理；
- 哈希计算。

### sys 很大

说明大量 CPU 消耗在内核，例如：

- 高频系统调用；
- 大量小包网络 I/O；
- page fault；
- 文件系统操作；
- 锁或调度开销；
- 数据在用户态和内核态频繁搬运。

### user + sys 接近 wall

对于单线程 CPU 密集程序，说明 CPU 大部分时间都在为它工作。

### user + sys 大于 wall

在多线程程序中完全可能：

```text
4 个线程各运行 1 秒
wall ≈ 1 秒
CPU time ≈ 4 秒
```

因为 CPU 时间会累计多个线程实际占用的处理器时间。

---

## 10.6 子进程时间与 `wait`

`tms_cutime` 和 `tms_cstime` 通常反映：

> 已经终止，并且调用者通过 wait 系列函数回收的子进程累计 CPU 时间。

如果子进程仍在运行，或者尚未被等待回收，其资源时间不一定已经计入父进程的子进程时间字段。

这再次说明：

```text
wait 不只是等待退出
还完成父进程对子进程终止信息和资源统计的接收
```

---

## 10.7 现代工程中的计时接口

`times()` 适合理解 Unix 进程 CPU 时间模型，但现代服务中还常使用：

### 单调时钟测量耗时

```c
clock_gettime(CLOCK_MONOTONIC, ...);
```

适合：

- 请求延迟；
- 超时；
- 性能基准；
- 不受系统时间调整影响的时间间隔。

### 进程 CPU 时间

```c
clock_gettime(CLOCK_PROCESS_CPUTIME_ID, ...);
```

### 线程 CPU 时间

```c
clock_gettime(CLOCK_THREAD_CPUTIME_ID, ...);
```

### 资源使用

```c
getrusage(...);
```

可以获得：

- CPU 时间；
- page fault；
- context switch；
- 其他资源统计。

工程选择：

```text
请求耗时：CLOCK_MONOTONIC
进程 CPU：CLOCK_PROCESS_CPUTIME_ID
线程 CPU：CLOCK_THREAD_CPUTIME_ID
传统父子进程统计：times
更完整资源统计：getrusage
```

---

# 11. 将后半章知识串成完整执行流程

下面以父进程启动一个低权限 worker 为例。

```text
1. 父进程准备参数、环境和 pipe
2. fork 创建子进程
3. 父子进程通过 pipe 建立确定同步
4. 子进程关闭不需要的文件描述符
5. 子进程设置 FD_CLOEXEC 或完成重定向
6. 子进程 setgid
7. 子进程 setuid，永久降低权限
8. 子进程 exec worker 程序
9. worker 以新程序映像运行，但 PID 不变
10. 调度器根据 nice/cgroup 等安排 CPU
11. 系统累计 user/sys CPU 时间
12. worker exit 或被信号终止
13. 父进程 waitpid 获取终止状态
14. 系统形成资源统计或会计信息
```

这条流程包含了后半章几乎所有核心知识。

---

# 12. 常见错误与最佳实践

## 12.1 假设父子进程执行顺序

错误：

```c
fork();
// 默认认为父进程先完成初始化
```

正确：

- 使用 pipe；
- 使用信号；
- 使用 socketpair；
- 通过明确同步协议建立顺序。

---

## 12.2 `exec` 失败后继续执行

错误：

```c
if (fork() == 0) {
    execv(path, argv);
}

parent_logic();
```

exec 失败时，子进程会继续执行 `parent_logic()`。

正确：

```c
if (fork() == 0) {
    execv(path, argv);
    _exit(127);
}
```

---

## 12.3 泄露文件描述符给新程序

错误：

```c
int secret_fd = open(...);
fork();
exec(...);
```

如果没有 `CLOEXEC`，目标程序可能继承 `secret_fd`。

正确：

```c
open(path, O_RDONLY | O_CLOEXEC);
```

只对明确要传递的描述符移除 close-on-exec。

---

## 12.4 通过 `system` 执行用户输入

错误：

```c
system(("grep " + user_input).c_str());
```

正确：

- 直接构造 `argv`；
- 使用 `execv/posix_spawn`；
- 不经过 Shell 解析。

---

## 12.5 高权限运行全部业务代码

错误：

```text
服务以 root 启动
所有网络解析、业务处理、插件加载都保持 root
```

更合理：

```text
完成特权初始化
    ↓
setgid
    ↓
setuid
    ↓
普通身份运行
```

或者拆分独立特权辅助进程。

---

## 12.6 把 nice 当作性能优化

nice 只能调整调度倾向，不能解决：

- 锁竞争；
- I/O 阻塞；
- 算法复杂度；
- 内存抖动；
- 系统调用过多；
- CPU 配额不足。

性能问题仍应通过监控、profiling 和资源隔离分析。

---

## 12.7 用 wall time 判断 CPU 消耗

请求耗时 10 秒，不代表消耗了 10 秒 CPU。

需要同时观察：

```text
wall time
user CPU time
system CPU time
I/O wait
调度和锁等待
```

---

# 13. 快速复习表

| 小节 | 核心问题 | 工程记忆 |
|---|---|---|
| 竞争条件 | 父子进程谁先执行 | 不依赖调度顺序，使用显式同步 |
| `exec` | 如何运行新程序 | 替换程序映像，PID 不变 |
| UID/GID | 程序以谁的权限运行 | 有效 ID 决定多数权限检查 |
| 解释器文件 | 脚本为何能直接执行 | `#!` 指定解释器，最终仍是 exec |
| `system` | 如何执行 Shell 命令 | 方便但有注入和信号复杂性 |
| 进程会计 | 如何记录进程资源 | 进程结束时形成离线统计 |
| 用户标识 | 谁登录、谁在运行 | `getlogin` 与 UID 查询含义不同 |
| 进程调度 | 如何影响 CPU 分配 | nice 是倾向，不是实时保证 |
| 进程时间 | 时间花在哪里 | 区分 wall、user、sys、child CPU |

---

# 14. 必须掌握的 API

## 进程同步和执行

```c
fork()
execv()
execvp()
execve()
fexecve()
waitpid()
```

## 身份和权限

```c
getuid()
geteuid()
getgid()
getegid()

setuid()
seteuid()
setgid()
setegid()
```

## 用户信息

```c
getlogin()
getpwuid()
```

## 调度

```c
nice()
getpriority()
setpriority()
```

## 进程时间

```c
times()
sysconf(_SC_CLK_TCK)
```

---

# 15. C++ 服务端工程师的掌握优先级

## 第一优先级：必须真正理解

1. `exec` 替换程序映像，但不创建新 PID；
2. `fork + exec + waitpid` 的完整进程启动模型；
3. `exec` 成功不返回，失败路径必须 `_exit`；
4. 文件描述符默认跨 exec 继承；
5. `FD_CLOEXEC/O_CLOEXEC` 的作用；
6. 不可信输入不能拼接给 `system`；
7. 父子进程执行顺序必须显式同步；
8. wall time 与 CPU time 的区别。

## 第二优先级：工程上需要认识

1. real/effective/saved UID 的关系；
2. 服务启动后永久降低权限；
3. shebang 的工作机制；
4. nice 只影响普通调度倾向；
5. `getlogin` 不适合所有后台服务；
6. 进程会计是退出时统计，不是实时监控。

## 第三优先级：了解即可

1. 传统进程会计文件格式；
2. 不同 Unix 对 nice 范围和调度细节的差异；
3. 解释器可选参数的跨平台差异；
4. 临时切换保存用户 ID 的复杂规则。

---

# 16. 本章后半部分的最终心智模型

可以把这些小节浓缩为五个问题。

## 1. 谁先运行

```text
竞争条件与同步
```

不要依赖 `fork` 后的调度顺序。

## 2. 运行什么程序

```text
exec
```

进程身份不变，程序映像被替换。

## 3. 以谁的权限运行

```text
UID / GID
```

多数权限检查取决于有效身份。

## 4. 获得多少 CPU

```text
nice / priority
```

只能影响调度倾向，不能提供实时保证。

## 5. 实际消耗了什么资源

```text
times / process accounting
```

区分现实耗时、用户态 CPU、内核态 CPU 和子进程 CPU。

最终完整模型：

```text
fork 创建进程
    ↓
同步确定执行时序
    ↓
配置 fd、环境和权限
    ↓
exec 替换程序
    ↓
调度器分配 CPU
    ↓
系统累计 CPU 和资源使用
    ↓
进程退出
    ↓
waitpid 回收并取得状态
```

掌握这条主线，就不再是在记忆零散 API，而是在理解 Unix 如何启动、约束、运行和回收一个生产进程。
