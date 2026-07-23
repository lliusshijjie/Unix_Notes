# 《UNIX 环境高级编程》第八章：进程控制（上）

> 本文总结第八章从“引言”到 `waitid` 之前（包含 `waitid`）的内容。  
> 学习目标不是背诵 API，而是建立一条完整的进程生命周期主线：
>
> ```text
> 识别进程 → fork 创建子进程 → 父子进程分别运行
>          → 子进程退出 → 父进程 wait/waitpid 回收
> ```

---

## 1. 本部分的整体知识架构

本部分主要包含以下内容：

1. 进程标识符：系统如何区分不同进程。
2. `fork`：如何复制当前进程并创建子进程。
3. `vfork`：一种历史性的特殊进程创建方式。
4. 进程终止：进程有哪些正常和异常退出方式。
5. `wait` / `waitpid`：父进程如何等待并回收子进程。
6. `waitid`：使用更结构化的信息获取子进程状态。

这些知识并不是相互独立的 API，而是在描述一个完整过程：

```text
父进程
   |
   | fork()
   v
父进程 + 子进程
   |
   | 子进程执行任务
   v
子进程调用 exit/_exit，或被信号终止
   |
   v
子进程进入“已终止但尚未回收”的状态
   |
   | 父进程调用 wait/waitpid/waitid
   v
内核释放子进程最后保留的进程信息
```

对服务端工程而言，本部分最重要的不是手工创建大量进程，而是理解：

- Shell、守护进程、进程池如何启动子进程；
- 为什么会出现僵尸进程；
- 为什么 `fork` 后的输出可能重复；
- 父子进程的内存和文件描述符到底是什么关系；
- 多进程服务如何回收退出的 worker；
- 为什么多线程程序中调用 `fork` 很危险。

---

# 2. 进程标识符

## 2.1 PID：进程的系统级身份

每个进程都有一个非负整数形式的进程 ID，类型通常是：

```c
pid_t
```

获取当前进程 PID：

```c
#include <unistd.h>

pid_t pid = getpid();
```

获取父进程 PID：

```c
pid_t ppid = getppid();
```

基本关系：

```text
父进程 PID = 1000
    |
    | fork()
    v
子进程 PID = 1001
子进程的 PPID = 1000
```

PID 在某个时刻唯一，但进程退出后，其 PID 以后可能被系统重新使用。因此：

> PID 是进程当前生命周期内的标识，不是永久身份。

---

## 2.2 常见特殊进程

不同 Unix 系统细节有所差异，但通常需要认识以下角色。

### PID 0

通常与内核调度或空闲进程有关，不是普通用户进程。

### PID 1

系统启动后创建的第一个用户态进程。传统 Unix 中通常称为 `init`，现代 Linux 上常见的是 `systemd`。

它承担的重要职责之一是：

> 接管失去父进程的孤儿进程，并回收其退出状态。

### PID 2

在部分 Unix/Linux 实现中与内核线程管理有关。其具体含义依赖系统实现，不应写死到业务逻辑中。

---

## 2.3 用户和组相关标识

常用函数：

```c
uid_t getuid(void);     // 实际用户 ID
uid_t geteuid(void);    // 有效用户 ID
gid_t getgid(void);     // 实际组 ID
gid_t getegid(void);    // 有效组 ID
```

理解重点：

- **实际用户 ID**：谁启动了这个进程；
- **有效用户 ID**：内核进行权限检查时，当前进程以谁的权限运行。

例如带有 set-user-ID 属性的程序，实际用户和有效用户可能不同。

对于普通服务端程序，需要知道：

- 进程读取文件、绑定端口、发送信号等权限检查，通常与有效用户 ID 有关；
- 服务通常先以高权限启动，再主动降低权限；
- 不应长期以 root 身份运行全部业务逻辑。

---

# 3. `fork`：创建子进程

## 3.1 函数原型

```c
#include <unistd.h>

pid_t fork(void);
```

`fork()` 调用一次，却会在两个进程中分别返回：

```c
pid_t pid = fork();

if (pid < 0) {
    // 创建失败
} else if (pid == 0) {
    // 子进程
} else {
    // 父进程，pid 是子进程 PID
}
```

返回值含义：

| 返回值 | 当前执行者 | 含义 |
|---:|---|---|
| `< 0` | 原进程 | 创建失败 |
| `0` | 子进程 | 当前代码运行在子进程中 |
| `> 0` | 父进程 | 返回值是新建子进程的 PID |

---

## 3.2 最关键的心智模型

`fork` 不是让子进程从 `main()` 重新开始执行，也不是调用某个“子进程入口函数”。

它真正做的是：

> 复制当前进程的执行环境，父子进程都从 `fork()` 返回的位置继续运行。

```text
fork 前：

父进程
  |
  | pid = fork();
  v

fork 后：

                     ┌─ 父进程：pid = child_pid
fork 返回位置 ────────┤
                     └─ 子进程：pid = 0
```

父子进程拥有相同的程序代码，但后续可以根据返回值走不同分支。

---

## 3.3 书中变量示例：为什么父子进程互不影响

典型示例结构如下：

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int globvar = 6;

int main(void)
{
    int var = 88;
    pid_t pid;

    printf("before fork\n");

    if ((pid = fork()) < 0) {
        perror("fork");
        exit(1);
    } else if (pid == 0) {
        globvar++;
        var++;
    } else {
        sleep(2);
    }

    printf("pid = %ld, globvar = %d, var = %d\n",
           (long)getpid(), globvar, var);

    return 0;
}
```

### `fork` 前

只有父进程：

```text
globvar = 6
var     = 88
```

### `fork` 刚刚结束时

父子进程中的变量初始值相同：

```text
父进程                   子进程
globvar = 6              globvar = 6
var     = 88             var     = 88
```

### 子进程修改变量后

```text
父进程                   子进程
globvar = 6              globvar = 7
var     = 88             var     = 89
```

父进程中的变量不会被子进程修改，因为它们拥有独立的虚拟地址空间。

这说明：

> `fork` 复制的是进程地址空间的逻辑视图，而不是让父子进程共享普通变量。

---

## 3.4 Copy-on-Write：为什么 `fork` 没有立即复制全部内存

从程序语义上看，父子进程各自拥有：

- 代码段；
- 全局数据；
- 堆；
- 栈；
- 标准 I/O 流及其用户态缓冲区。

但现代 Unix 通常使用写时复制（Copy-on-Write，COW）：

```text
fork 刚结束：
父子进程页表暂时指向相同物理页
这些页面被设置为只读

某一方尝试写入：
触发缺页异常
内核复制对应页面
写入方修改自己的副本
```

因此：

- `fork` 不会立即复制整个进程的所有物理内存；
- 只读页面可以继续共享；
- 被修改的页面才真正复制；
- 仍然存在页表复制、内核进程结构创建等成本。

对服务端的实际意义：

- `fork + exec` 通常较高效，因为子进程很快会用新程序替换地址空间；
- 大型多线程进程直接 `fork` 仍然可能产生明显延迟；
- fork 后大量修改内存，会破坏 COW，产生真实复制成本。

---

## 3.5 父子进程继承了什么

子进程通常会继承父进程的许多属性，例如：

- 打开的文件描述符；
- 当前工作目录；
- 根目录；
- 文件模式创建屏蔽字 `umask`；
- 用户 ID、组 ID；
- 环境变量；
- 资源限制；
- 信号屏蔽字；
- 已安装的信号处理方式；
- 调度属性；
- 进程组和会话关系。

但父子进程也有明显差异：

- PID 不同；
- 父进程 ID 不同；
- `fork()` 返回值不同；
- 子进程累计 CPU 时间通常从 0 开始；
- 未决信号集合不直接复制为相同状态；
- 文件锁等特性需结合具体锁类型判断。

学习时无需死记全部属性，重点把它们归为两类：

```text
用户态地址空间：逻辑复制，之后互相独立
内核对象引用：部分复制引用关系，可能继续共享底层对象
```

---

## 3.6 文件描述符：复制描述符表，共享打开文件对象

这是 `fork` 最重要的工程知识之一。

父进程执行：

```c
int fd = open("data.txt", O_WRONLY);
```

`fork` 后可以抽象为：

```text
父进程文件描述符表             子进程文件描述符表
fd = 3                         fd = 3
   |                              |
   +--------------+---------------+
                  |
                  v
        内核打开文件描述对象
        - 当前文件偏移量
        - 文件状态标志
        - vnode/inode 引用
```

因此：

- 父子进程各有自己的文件描述符表；
- 但对应描述符通常指向同一个内核打开文件描述对象；
- 二者共享文件偏移量和部分文件状态标志。

示例：

```c
int fd = open("log.txt",
              O_WRONLY | O_CREAT | O_TRUNC,
              0644);

pid_t pid = fork();

if (pid == 0) {
    write(fd, "child\n", 6);
    _exit(0);
}

write(fd, "parent\n", 7);
waitpid(pid, NULL, 0);
close(fd);
```

文件内容可能是：

```text
parent
child
```

也可能是：

```text
child
parent
```

原因是：

- 父子进程共享底层文件偏移；
- 但谁先执行没有确定顺序。

工程上应注意：

- 共享文件描述符不等于拥有稳定写入顺序；
- 多进程写日志应考虑 `O_APPEND`、单写者日志进程或可靠 IPC；
- fork 后不需要的描述符应及时关闭；
- socket 和 pipe 的端点也会被继承，这正是后续进程间通信的基础。

---

## 3.7 标准 I/O 缓冲区为什么会导致重复输出

考虑：

```c
printf("before fork");
fork();
```

如果 `printf` 的内容仍留在用户态缓冲区中，`fork` 会把这个缓冲区也复制到子进程：

```text
fork 前：

父进程 stdout 缓冲区
"before fork"

fork 后：

父进程缓冲区              子进程缓冲区
"before fork"             "before fork"
```

父子进程正常退出时，各自刷新一遍缓冲区，于是可能得到：

```text
before forkbefore fork
```

这不是 `printf` 被执行了两次，而是：

> 一次 `printf` 产生的未刷新缓冲区，被 `fork` 复制成两份。

### 终端与文件重定向的差异

stdout 连接终端时通常是行缓冲：

```c
printf("before fork\n");
```

换行可能使缓冲区在 `fork` 前就被刷新，因此只输出一次。

重定向到文件：

```bash
./a.out > output.txt
```

stdout 常变为全缓冲，数据可能仍未刷新，因此出现重复内容。

### 工程处理方式

在 `fork` 前主动刷新：

```c
fflush(NULL);
pid_t pid = fork();
```

或者在子进程 `exec` 失败后调用 `_exit()`，避免再次刷新从父进程复制来的 stdio 缓冲区。

---

## 3.8 调度顺序是不确定的

`fork` 后，父进程和子进程谁先运行没有保证：

```c
pid_t pid = fork();

if (pid == 0) {
    printf("child\n");
} else {
    printf("parent\n");
}
```

可能输出：

```text
parent
child
```

也可能输出：

```text
child
parent
```

所以：

> 不能使用“通常父进程先运行”或“通常子进程先运行”来设计正确性。

书中示例使用 `sleep()`，通常只是为了让现象更容易观察，不是可靠同步机制。

严格等待子进程退出应使用：

```c
waitpid(pid, NULL, 0);
```

如果需要在执行中间同步，则应使用：

- pipe；
- socketpair；
- 信号；
- 共享内存和同步原语；
- 信号量；
- eventfd。

---

## 3.9 `fork` 失败的常见原因

`fork()` 返回 `-1` 时，常见原因包括：

- 当前用户进程数量达到限制；
- 系统可创建进程数量达到限制；
- 内核资源不足；
- 内存或页表相关资源不足。

正确处理：

```c
pid_t pid = fork();

if (pid < 0) {
    perror("fork");
    // 根据业务决定重试、降级或退出
}
```

服务端程序不应无上限地创建进程。常见方案是：

- 固定大小的 pre-fork 进程池；
- 线程池；
- 任务队列；
- 进程数量限制和退避重试。

---

# 4. `vfork`

## 4.1 `vfork` 的设计目的

```c
#include <unistd.h>

pid_t vfork(void);
```

`vfork` 是一种历史上的优化方式，最初用于解决早期系统中 `fork` 复制地址空间成本过高的问题。

它假定子进程创建后会立即调用：

```c
exec...
```

或：

```c
_exit(...)
```

典型语义：

- 子进程暂时在父进程地址空间中运行；
- 父进程通常会被挂起；
- 子进程调用 `exec` 或 `_exit` 后，父进程才恢复；
- 子进程不能随意修改父进程地址空间和栈。

---

## 4.2 为什么 `vfork` 危险

由于父子进程可能暂时共享同一个地址空间，子进程进行普通 C/C++ 操作可能破坏父进程状态：

```c
int value = 10;

pid_t pid = vfork();

if (pid == 0) {
    value = 20;   // 可能直接影响父进程
    _exit(0);
}
```

尤其危险的操作包括：

- 返回当前函数；
- 修改栈变量；
- 调用 `exit()`；
- 调用复杂标准库；
- 调用可能修改全局状态或堆的函数；
- 在 C++ 中创建或析构复杂对象。

子进程通常只应：

```text
vfork → 少量安全准备 → exec
```

如果 `exec` 失败：

```c
if (execlp("program", "program", NULL) == -1) {
    _exit(127);
}
```

---

## 4.3 现代工程如何看待 `vfork`

现代系统已经通过 COW 大幅降低了 `fork` 的内存复制成本，因此普通业务代码几乎不应主动使用 `vfork`。

建议：

- 学习它是为了理解历史和进程地址空间语义；
- 不要把它当作日常性能优化工具；
- 启动外部程序时可优先考虑 `posix_spawn()`；
- 除非明确理解目标平台实现，否则不要在 C++ 服务中直接使用 `vfork`。

---

# 5. 进程终止

## 5.1 正常终止方式

进程可以通过多种方式正常结束。

### 从 `main` 返回

```c
int main(void)
{
    return 0;
}
```

效果近似于：

```c
exit(0);
```

### 调用 `exit`

```c
#include <stdlib.h>

exit(status);
```

`exit` 会执行用户态清理工作，例如：

- 调用 `atexit` 注册的函数；
- 刷新并关闭标准 I/O 流；
- 最终把退出状态交给内核。

### 调用 `_exit`

```c
#include <unistd.h>

_exit(status);
```

`_exit` 直接进入内核终止流程，不执行 stdio 刷新和 `atexit` 处理函数。

### 最后一个线程正常返回

在多线程进程中，最后一个线程结束后，进程也会终止。

---

## 5.2 异常终止方式

常见异常终止包括：

- 调用 `abort()`；
- 收到默认行为为终止进程的信号；
- 最后一个线程对取消请求作出响应；
- 发生严重运行时错误并被信号终止。

例如：

```c
abort();
```

通常会使进程收到 `SIGABRT`。

非法内存访问常导致：

```text
SIGSEGV
```

父进程可以通过 `wait/waitpid` 获取子进程的异常终止原因。

---

## 5.3 `exit` 与 `_exit` 的关键区别

### `exit`

```text
执行 atexit 处理函数
刷新 stdio 用户态缓冲区
关闭标准 I/O 流
进入内核终止流程
```

### `_exit`

```text
不执行用户态清理
直接进入内核终止流程
```

这一区别在 `fork` 后非常重要。

典型写法：

```c
pid_t pid = fork();

if (pid == 0) {
    execlp("ls", "ls", "-l", NULL);

    // exec 成功不会返回
    perror("execlp");
    _exit(127);
}
```

为什么失败路径使用 `_exit`：

1. 防止子进程重复刷新父进程复制来的 stdio 缓冲区；
2. 防止子进程执行父进程注册的 `atexit` 函数；
3. 避免重复释放或修改仅适合父进程执行的用户态状态；
4. 多线程程序中，`exit` 可能进入并不安全的复杂库逻辑。

---

## 5.4 退出状态

```c
exit(7);
```

其中 `7` 是进程退出状态。

父进程可以通过：

```c
wait(&status);
```

或：

```c
waitpid(pid, &status, 0);
```

获取编码后的状态，并通过宏读取真实退出码。

Shell 中常见：

```bash
echo $?
```

会显示前一个命令的退出状态。

约定上：

- `0` 通常表示成功；
- 非 `0` 表示不同类型的失败。

---

## 5.5 退出时内核会做什么

进程终止时，内核通常会：

- 关闭该进程所有打开的文件描述符；
- 释放地址空间；
- 释放大部分内核资源；
- 保存少量终止信息；
- 向父进程发送 `SIGCHLD`；
- 等待父进程获取退出状态。

被暂时保留的信息通常包括：

- PID；
- 终止状态；
- 资源使用统计；
- 被哪个信号终止。

这就是僵尸进程存在的原因：

> 子进程已经不再运行，但父进程尚未读取其终止信息。

---

# 6. 孤儿进程与僵尸进程

## 6.1 孤儿进程

```text
父进程先退出
子进程仍在运行
```

此时子进程会被系统中的其他进程接管。在传统模型中通常由 PID 1 接管，现代 Linux 中也可能由 subreaper 接管。

孤儿进程本身并不等于资源泄漏。

---

## 6.2 僵尸进程

```text
子进程已经退出
父进程仍然存在
父进程尚未调用 wait 系列函数
```

僵尸进程：

- 不再执行代码；
- 地址空间大部分已经释放；
- 仍占用 PID 和进程表项；
- 保留退出状态等待父进程读取。

大量僵尸进程可能造成：

- PID 资源耗尽；
- 无法继续创建新进程；
- 服务稳定性下降。

解决方式：

- 父进程调用 `wait/waitpid/waitid`；
- 正确处理 `SIGCHLD`；
- 设计专门的子进程管理逻辑。

---

# 7. `wait` 与 `waitpid`

## 7.1 为什么需要等待函数

子进程退出后，父进程通常需要完成两件事：

1. 获得子进程如何结束；
2. 通知内核可以彻底回收子进程残留信息。

因此 `wait` 系列函数的作用不仅是“等待”：

> 它们同时承担子进程状态获取和僵尸进程回收。

---

## 7.2 `wait`

函数原型：

```c
#include <sys/wait.h>

pid_t wait(int *status);
```

基本用法：

```c
int status;
pid_t pid = wait(&status);
```

行为：

- 已经有子进程终止：立即返回该子进程 PID；
- 有子进程但尚未终止：阻塞；
- 没有任何可等待的子进程：返回 `-1`，`errno` 通常为 `ECHILD`；
- 被信号中断：返回 `-1`，`errno` 可能为 `EINTR`。

不关心退出状态：

```c
wait(NULL);
```

---

## 7.3 `waitpid`

函数原型：

```c
pid_t waitpid(pid_t pid, int *status, int options);
```

`waitpid` 是更灵活的等待接口。

最常见写法：

```c
int status;
pid_t ret = waitpid(child_pid, &status, 0);
```

含义：

> 阻塞等待指定的子进程终止。

---

## 7.4 `waitpid` 的 `pid` 参数

### `pid > 0`

等待指定 PID 的子进程：

```c
waitpid(child_pid, &status, 0);
```

### `pid == -1`

等待任意子进程：

```c
waitpid(-1, &status, 0);
```

与 `wait()` 的基本行为相似。

### `pid == 0`

等待与当前进程属于同一进程组的任意子进程。

### `pid < -1`

等待进程组 ID 等于 `-pid` 的任意子进程。

初学阶段最需要掌握：

```text
pid > 0   等指定子进程
pid == -1 等任意子进程
```

---

## 7.5 `waitpid` 返回值

```c
pid_t ret = waitpid(pid, &status, options);
```

| 返回值 | 含义 |
|---:|---|
| `> 0` | 返回发生状态变化的子进程 PID |
| `0` | 使用 `WNOHANG`，但当前没有符合条件的子进程状态变化 |
| `-1` | 调用失败 |

常见错误：

- `ECHILD`：不存在符合条件的子进程；
- `EINTR`：等待过程被信号中断；
- `EINVAL`：选项参数非法。

阻塞等待时应考虑 `EINTR`：

```c
int status;
pid_t ret;

do {
    ret = waitpid(pid, &status, 0);
} while (ret < 0 && errno == EINTR);

if (ret < 0) {
    perror("waitpid");
}
```

---

## 7.6 `options` 参数

### `0`

默认阻塞等待：

```c
waitpid(pid, &status, 0);
```

### `WNOHANG`

非阻塞检查：

```c
pid_t ret = waitpid(pid, &status, WNOHANG);
```

返回结果：

```c
if (ret > 0) {
    // 子进程状态已变化并被处理
} else if (ret == 0) {
    // 子进程仍在运行
} else {
    // 出错
}
```

### `WUNTRACED`

除了终止状态，也报告被暂停的子进程。

### `WCONTINUED`

报告此前暂停、现在已继续运行的子进程。

普通服务端最常见的是：

```text
options = 0
WNOHANG
```

---

## 7.7 如何解析 `status`

`status` 不是可以直接打印的退出码，而是编码后的进程状态。

### 正常退出

```c
if (WIFEXITED(status)) {
    int code = WEXITSTATUS(status);
}
```

- `WIFEXITED(status)`：是否正常退出；
- `WEXITSTATUS(status)`：获取退出码。

### 被信号终止

```c
if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);
}
```

- `WIFSIGNALED(status)`：是否被信号终止；
- `WTERMSIG(status)`：获取终止信号编号。

部分系统支持：

```c
WCOREDUMP(status)
```

用于判断是否产生 core dump，但可移植性需谨慎。

### 被暂停

```c
if (WIFSTOPPED(status)) {
    int sig = WSTOPSIG(status);
}
```

### 恢复运行

```c
if (WIFCONTINUED(status)) {
    // 子进程恢复执行
}
```

通用解析函数：

```c
void print_child_status(int status)
{
    if (WIFEXITED(status)) {
        printf("normal termination, exit status = %d\n",
               WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        printf("abnormal termination, signal = %d\n",
               WTERMSIG(status));
    } else if (WIFSTOPPED(status)) {
        printf("child stopped, signal = %d\n",
               WSTOPSIG(status));
    } else if (WIFCONTINUED(status)) {
        printf("child continued\n");
    }
}
```

必须先判断状态类别，再读取对应字段：

```c
if (WIFEXITED(status)) {
    printf("%d\n", WEXITSTATUS(status));
}
```

不能在未知状态类型时直接调用 `WEXITSTATUS()`。

---

## 7.8 书中不同终止方式示例的意义

书中通常会创建多个子进程，让它们分别：

1. 调用 `exit(7)` 正常退出；
2. 调用 `abort()` 被 `SIGABRT` 终止；
3. 触发除零或非法访问等异常。

它想说明的不是记忆具体信号编号，而是建立以下流程：

```text
子进程结束
   |
   v
内核把结束原因编码到 status
   |
   v
父进程 wait/waitpid 获取 status
   |
   v
WIFEXITED / WIFSIGNALED 判断类别
   |
   v
WEXITSTATUS / WTERMSIG 获取详情
```

这也是进程管理器、Shell 和 worker supervisor 判断任务成功或失败的基础。

---

## 7.9 `wait` 与 `waitpid` 的关系

`wait` 可以近似理解为：

```c
waitpid(-1, status, 0);
```

对比：

| 需求 | 推荐接口 |
|---|---|
| 等待任意一个子进程 | `wait()` 或 `waitpid(-1, ..., 0)` |
| 等待指定子进程 | `waitpid(child_pid, ..., 0)` |
| 非阻塞检查子进程 | `waitpid(..., WNOHANG)` |
| 处理多个 worker 退出 | 循环 `waitpid(-1, ..., WNOHANG)` |

工程上通常优先使用 `waitpid`，因为语义更明确，能力也更完整。

---

## 7.10 批量回收子进程

服务器父进程可能管理多个 worker。多个子进程可能几乎同时退出，因此不能只回收一次。

典型模式：

```c
for (;;) {
    int status;
    pid_t pid = waitpid(-1, &status, WNOHANG);

    if (pid > 0) {
        // 成功回收一个子进程
        print_child_status(status);
        continue;
    }

    if (pid == 0) {
        // 仍有子进程，但目前没有退出
        break;
    }

    if (errno == EINTR) {
        continue;
    }

    if (errno == ECHILD) {
        // 已经没有任何子进程
        break;
    }

    perror("waitpid");
    break;
}
```

必须循环的原因：

- 多个子进程可能同时退出；
- 标准信号通常不是可靠的逐个排队通知；
- 一次 `SIGCHLD` 到达时，可能已有多个僵尸子进程。

因此常见原则是：

```c
while (waitpid(-1, &status, WNOHANG) > 0) {
    // reap all terminated children
}
```

---

# 8. `waitid`

## 8.1 函数作用

`waitid` 提供比 `wait/waitpid` 更结构化的子进程状态信息。

典型原型：

```c
#include <sys/wait.h>

int waitid(idtype_t idtype,
           id_t id,
           siginfo_t *infop,
           int options);
```

与 `waitpid` 返回一个编码后的整数状态不同，`waitid` 将信息写入：

```c
siginfo_t
```

这使调用者可以更直接地读取：

- 哪个子进程发生状态变化；
- 为什么发生状态变化；
- 退出状态或信号编号。

---

## 8.2 `idtype` 与 `id`

常见选择：

### `P_PID`

等待指定 PID：

```c
waitid(P_PID, child_pid, &info, WEXITED);
```

### `P_PGID`

等待指定进程组中的子进程。

### `P_ALL`

等待任意子进程：

```c
waitid(P_ALL, 0, &info, WEXITED);
```

---

## 8.3 常见选项

`waitid` 需要明确指定关注哪些状态。

### `WEXITED`

等待已经终止的子进程。

### `WSTOPPED`

等待已经停止的子进程。

### `WCONTINUED`

等待恢复运行的子进程。

### `WNOHANG`

非阻塞检查。

### `WNOWAIT`

获取子进程状态，但暂时不回收它。后续仍可再次等待该子进程。

这是 `waitid` 相对独特的能力之一，适合“先观察状态、稍后正式回收”的特殊场景。

---

## 8.4 基本示例

```c
siginfo_t info = {0};

if (waitid(P_PID,
           child_pid,
           &info,
           WEXITED) == -1) {
    perror("waitid");
    return 1;
}

printf("child pid = %ld\n", (long)info.si_pid);
printf("status    = %d\n", info.si_status);
printf("code      = %d\n", info.si_code);
```

需要结合 `si_code` 判断 `si_status` 表示退出码还是信号编号。

常见 `si_code` 可能包括：

- `CLD_EXITED`：正常退出；
- `CLD_KILLED`：被信号杀死；
- `CLD_DUMPED`：因信号终止并产生 core dump；
- `CLD_STOPPED`：被暂停；
- `CLD_CONTINUED`：恢复运行。

示例：

```c
switch (info.si_code) {
case CLD_EXITED:
    printf("normal exit, status=%d\n", info.si_status);
    break;

case CLD_KILLED:
case CLD_DUMPED:
    printf("killed by signal=%d\n", info.si_status);
    break;

default:
    break;
}
```

---

## 8.5 `waitpid` 与 `waitid` 如何选择

普通业务代码：

```text
优先使用 waitpid
```

原因：

- 接口简单；
- 资料和范例多；
- 足以覆盖绝大多数子进程管理需求；
- `WNOHANG` 可以实现非阻塞批量回收。

需要更详细、结构化状态信息，或需要 `WNOWAIT` 时：

```text
考虑 waitid
```

---

# 9. `fork + exec + waitpid`：Unix 创建程序的完整模型

虽然 `exec` 在本章后续才会详细介绍，但必须先知道 `fork` 的主要实际用途。

```c
pid_t pid = fork();

if (pid < 0) {
    perror("fork");
    exit(1);
}

if (pid == 0) {
    execlp("ls", "ls", "-l", NULL);

    // exec 成功不会返回
    perror("execlp");
    _exit(127);
}

int status;

while (waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) {
        continue;
    }

    perror("waitpid");
    exit(1);
}

if (WIFEXITED(status)) {
    printf("child exit code = %d\n",
           WEXITSTATUS(status));
}
```

执行流程：

```text
Shell 或父进程
    |
    | fork()
    +-------------------------+
    |                         |
    v                         v
父进程                     子进程
waitpid()                  exec("ls")
    |                         |
    |                      执行 ls
    |                         |
    |                      进程退出
    |                         |
    +------ 获取退出状态 <----+
    |
继续运行
```

这就是 Unix 的核心思想之一：

```text
fork：创建新的执行实体
exec：用新程序替换子进程
waitpid：父进程等待并回收子进程
```

---

# 10. 多线程服务中的 `fork` 风险

单线程程序中，`fork` 模型相对清晰。

但一个多线程进程调用 `fork` 时：

- 父进程保留全部线程；
- 子进程只保留调用 `fork` 的那个线程；
- 其他线程不会出现在子进程；
- 但内存中由其他线程持有的锁状态可能被复制。

例如：

```text
父进程：
线程 A 持有 mutex
线程 B 调用 fork

子进程：
只剩线程 B
mutex 仍显示为已加锁
但线程 A 已不存在
```

子进程再次尝试获得该锁时，可能永久死锁。

因此多线程程序中，fork 后、exec 前只能非常谨慎地执行代码。POSIX 原则上只允许调用 async-signal-safe 函数。

C++ 服务中尤其危险的操作：

- `std::cout`；
- 日志库；
- `new/delete`；
- `malloc/free`；
- `std::mutex`；
- 复杂容器操作；
- 全局对象析构；
- 依赖后台线程的库。

工程建议：

1. 在线程启动之前完成 fork；
2. 使用 pre-fork worker 模型；
3. fork 后子进程尽快 exec；
4. 评估 `posix_spawn()`；
5. 不要在线程池已经运行后随意 fork。

---

# 11. 服务端常见模型

## 11.1 Pre-fork 进程池

父进程提前创建固定数量 worker：

```text
master
  ├── worker 1
  ├── worker 2
  ├── worker 3
  └── worker 4
```

父进程职责：

- 创建 worker；
- 监听 `SIGCHLD`；
- 使用 `waitpid` 回收退出 worker；
- 根据策略重新拉起；
- 统一处理配置重载和优雅退出。

优点：

- worker 相互隔离；
- 单 worker 崩溃不直接破坏其他 worker；
- 可以利用多核；
- 故障恢复模型清晰。

代价：

- 跨进程共享状态困难；
- 内存开销通常高于线程；
- IPC 更复杂；
- 文件描述符继承必须精确管理。

---

## 11.2 外部命令执行

服务启动外部程序时，经典流程：

```text
flush stdio
    |
fork
    |
子进程关闭无关 fd
    |
设置重定向
    |
exec
    |
父进程 waitpid
```

需要特别处理：

- `exec` 失败后的 `_exit`；
- 标准输入输出重定向；
- 超时与强制终止；
- 关闭不需要继承的文件描述符；
- 防止 shell 注入；
- 获取退出状态和 stderr。

---

# 12. 常见错误与最佳实践

## 12.1 使用 `sleep` 猜测顺序

错误：

```c
if (pid > 0) {
    sleep(1);
}
```

`sleep` 只能让某种顺序更可能出现，不能保证同步。

正确：

```c
waitpid(pid, NULL, 0);
```

---

## 12.2 忘记回收子进程

父进程长期运行却不执行 wait 系列函数，会积累僵尸进程。

正确做法：

- 对明确子进程使用 `waitpid(child_pid, ...)`；
- 对 worker 集合使用循环 `waitpid(-1, ..., WNOHANG)`；
- 结合 `SIGCHLD` 触发回收。

---

## 12.3 fork 后父子进程同时执行后续代码

错误结构：

```c
fork();
start_server();
```

此时父子进程都会执行 `start_server()`。

正确结构：

```c
pid_t pid = fork();

if (pid < 0) {
    // error
} else if (pid == 0) {
    // child path
} else {
    // parent path
}
```

---

## 12.4 子进程 exec 失败后继续执行父进程逻辑

错误：

```c
if (fork() == 0) {
    execlp("cmd", "cmd", NULL);
}

// exec 失败后子进程会继续走到这里
run_parent_logic();
```

正确：

```c
if (fork() == 0) {
    execlp("cmd", "cmd", NULL);
    _exit(127);
}
```

---

## 12.5 fork 前未处理 stdio 缓冲区

可能导致重复输出。

建议：

```c
fflush(NULL);
pid_t pid = fork();
```

但也应认识到，复杂多线程程序中全局刷新本身可能涉及锁，因此最根本的策略仍是规范 fork 时机，并尽快 exec。

---

## 12.6 认为普通变量能用于父子进程通信

```c
int ready = 0;

if (fork() == 0) {
    ready = 1;
}
```

父进程仍会看到自己的 `ready == 0`。

正确 IPC：

- pipe；
- Unix domain socket；
- socketpair；
- 共享内存；
- 消息队列；
- 信号。

---

## 12.7 只执行一次非阻塞 `waitpid`

错误：

```c
waitpid(-1, &status, WNOHANG);
```

如果多个子进程已经退出，只回收一个。

正确：

```c
while (waitpid(-1, &status, WNOHANG) > 0) {
}
```

---

# 13. 本部分核心知识图

```text
                     进程标识
              PID / PPID / UID / GID
                         |
                         v
父进程 ---------------- fork ----------------+
  |                                          |
  | fork 返回 child_pid                      | fork 返回 0
  |                                          |
  v                                          v
父进程继续运行                           子进程继续运行
  |                                      修改独立内存副本
  |                                      共享部分内核对象引用
  |                                          |
  |                                      exit / _exit
  |                                      或被信号终止
  |                                          |
  |                                          v
  |                                       僵尸状态
  |                                          |
  +--------- wait / waitpid / waitid <-------+
                         |
                         v
                  获取终止信息并回收
```

---

# 14. 快速复习清单

## `fork`

- 调用一次，在父子两个进程中返回；
- 父进程返回子进程 PID；
- 子进程返回 0；
- 父子从 `fork` 返回处继续执行；
- 普通变量逻辑上独立；
- 底层通常使用 COW；
- 文件描述符表复制，但底层打开文件对象可能共享；
- stdio 缓冲区会被复制，可能导致重复输出；
- 父子谁先运行没有保证。

## `vfork`

- 子进程临时借用父进程地址空间；
- 父进程通常暂停；
- 子进程只能尽快 `exec` 或 `_exit`；
- 现代普通业务代码不建议直接使用。

## 进程终止

- `return main`、`exit`：执行用户态清理；
- `_exit`：直接进入内核终止；
- fork 后 exec 失败通常使用 `_exit`；
- 子进程终止后可能暂时成为僵尸。

## `wait`

- 等待任意一个子进程；
- 获取终止状态；
- 回收僵尸进程。

## `waitpid`

- 可以等待指定子进程；
- 支持 `WNOHANG`；
- 应处理 `EINTR`；
- 多子进程回收应循环调用。

## 状态解析

```c
WIFEXITED(status)
WEXITSTATUS(status)

WIFSIGNALED(status)
WTERMSIG(status)

WIFSTOPPED(status)
WSTOPSIG(status)

WIFCONTINUED(status)
```

## `waitid`

- 通过 `siginfo_t` 返回结构化信息；
- 支持 `P_PID`、`P_PGID`、`P_ALL`；
- 支持 `WNOWAIT`；
- 普通需求通常仍优先使用 `waitpid`。

---

# 15. 最终应形成的工程理解

本部分最重要的不是记住几个函数原型，而是理解下面这条闭环：

```text
fork 创建子进程
    ↓
父子进程拥有独立执行流
    ↓
子进程执行任务或 exec 新程序
    ↓
子进程通过 exit/_exit 或信号终止
    ↓
内核暂存退出信息
    ↓
父进程使用 wait/waitpid/waitid 获取状态
    ↓
子进程被彻底回收
```

对 C++ 服务端工程师而言，可以将三类接口记为：

```text
fork      ：创建执行流
exit      ：结束执行流
waitpid   ：管理并回收执行流
```

而本部分最值得反复确认的四个工程问题是：

1. fork 后父子进程的普通内存为什么互不影响；
2. 为什么文件描述符对应的底层内核对象可能继续共享；
3. 为什么 stdio 缓冲区会导致重复输出；
4. 为什么父进程必须正确调用 wait 系列函数回收子进程。

掌握这四点，才算真正理解了第八章进程控制的前半部分。
