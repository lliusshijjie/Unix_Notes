# Unix 文件 I/O：服务端工程实践总结

> 这份笔记不是 APUE 第 3、4 章 API 清单，而是从 C++ 服务端工程角度总结 Unix 文件 I/O 真正需要掌握的模型、场景、最佳实践和常见坑。

---

## 1. 核心认知：不要背 API，要掌握模型

学习 Unix 文件 I/O，最重要的不是记住每个系统调用的参数，而是建立这几个模型：

```text
fd -> open file description -> inode
路径名 -> 目录项 -> inode
```

### 1.1 fd 是服务端资源的统一句柄

在 Unix/Linux 里，很多资源最终都表现为 fd：

```text
普通文件
socket
pipe
eventfd
timerfd
signalfd
epoll fd
设备文件
终端
```

所以文件 I/O 不是只服务于“读写磁盘文件”，而是在训练你理解服务端里最基础的资源抽象：

> 服务端程序通过 fd 和内核对象打交道。

网络编程里的 socket、epoll，本质上也延续了这一套 fd 模型。

### 1.2 fd 只是进程内的小整数

fd 不是资源本体，它只是进程 fd 表里的一个索引。真正的资源在内核中。

这带来一个很重要的工程坑：

```text
fd 会被复用
```

如果一个旧 fd 被 close 后，新 socket 可能马上复用同一个整数。  
如果 epoll 里还残留旧 fd 的事件，就可能出现“事件打到新连接上”的诡异 bug。

所以服务端里一定要保证：

```text
fd 生命周期
epoll 注册/注销
对象所有权
```

三者一致。

---

## 2. fd 表、open file description 与 inode

### 2.1 open 与 dup 的区别

```cpp
int fd1 = open("a.txt", O_RDONLY);
int fd2 = open("a.txt", O_RDONLY);
```

这通常会得到两个独立的 open file description，因此文件偏移量互不影响。

但：

```cpp
int fd1 = open("a.txt", O_RDONLY);
int fd2 = dup(fd1);
```

`fd1` 和 `fd2` 指向同一个 open file description，因此共享文件偏移量。

这点在服务端里非常重要，尤其涉及：

```text
fork 后父子进程共享 fd
dup/dup2 实现重定向
多线程/多进程写同一个 fd
日志文件、管道、socket 生命周期
```

### 2.2 pread / pwrite 的价值

`read/write` 会使用并推进当前文件偏移量。  
`pread/pwrite` 则在指定 offset 读写，不修改共享 offset。

所以在多线程随机读写文件时，`pread/pwrite` 通常比：

```cpp
lseek(fd, offset, SEEK_SET);
read(fd, buf, len);
```

更安全。

工程场景包括：

```text
存储引擎页读写
索引文件随机访问
大文件分块读取
多线程读取同一个文件
```

---

## 3. 路径不是文件本体：目录项与 inode

Unix 文件系统里，路径名不是文件本体。

```text
路径名 /data/a.txt
    ↓
目录项：a.txt -> inode number
    ↓
inode：权限、大小、时间戳、链接数、数据块位置
```

所以：

```text
文件名只是目录中的一条映射
inode 才更接近文件本体
```

这可以解释很多线上问题。

### 3.1 unlink 的真实语义

```cpp
unlink("server.log");
```

不是立刻销毁文件内容，而是删除一个目录项。

只有当：

```text
硬链接数为 0
并且没有任何进程持有该文件 fd
```

文件数据块才会真正释放。

这就是为什么线上经常出现：

```text
rm 掉日志文件后，df 看到磁盘空间仍然没释放
```

原因通常是服务进程仍然持有这个 deleted 文件的 fd。

排查方式：

```bash
lsof | grep deleted
ls -l /proc/<pid>/fd
```

---

## 4. 服务端高频应用场景

### 4.1 日志系统

日志是文件 I/O 在服务端里最常见的场景。

常见调用：

```cpp
open("server.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
write(fd, data, len);
```

工程关注点：

```text
是否使用 O_APPEND
多线程写日志是否会交错
日志是否需要 fsync
日志轮转后是否需要 reopen
日志文件被 rm 后 fd 是否仍然存在
磁盘满 ENOSPC 怎么处理
```

注意：

```text
write 成功不等于数据真正落盘
```

如果日志是审计日志、交易日志、WAL，这个区别非常关键。

---

### 4.2 配置文件读取与热更新

服务启动时会读配置：

```text
server.conf
config.yaml
config.json
.env
```

涉及：

```text
open
read
stat
rename
```

配置热更新时，重点不是“怎么读文件”，而是：

```text
如何避免读到半写入文件？
如何保证替换过程原子？
如何处理权限和路径被替换？
```

推荐模式：

```text
1. 写 config.tmp
2. fsync(config.tmp)
3. close(config.tmp)
4. rename(config.tmp, config.json)
```

这样读者要么看到旧配置，要么看到新配置，不会看到写了一半的配置。

---

### 4.3 pid 文件 / lock 文件

很多服务会使用：

```text
/var/run/server.pid
```

防止重复启动。

常见流程：

```text
1. open pid 文件
2. 使用 fcntl 加文件锁
3. 写入当前进程 pid
4. 服务退出后自动释放锁
```

它解决的是：

```text
同一个服务不能启动多个实例
```

比简单判断 pid 文件是否存在更可靠，因为进程异常退出后，文件可能残留，但文件锁会随着进程退出自动释放。

---

### 4.4 静态文件服务

HTTP 服务返回静态资源时会涉及文件 I/O：

```text
html
css
js
image
video
```

基础路径：

```text
open
fstat
read/write
```

高性能路径：

```text
sendfile
mmap
```

其中 `sendfile` 可以实现：

```text
文件 fd -> socket fd
```

减少用户态拷贝，是典型的服务端性能优化手段。

---

### 4.5 上传、下载与临时文件

用户上传文件时，不建议直接写正式路径。

推荐流程：

```text
1. 写入临时文件
2. 写完后 fsync
3. rename 到正式路径
4. 失败时 unlink 临时文件
```

这可以避免：

```text
写到一半服务崩溃
正式文件残缺
其他请求读到半成品
```

---

### 4.6 本地缓存、索引与存储

一些服务会在本地维护：

```text
模型文件
索引文件
snapshot
checkpoint
WAL
本地 KV 缓存
```

这类场景更接近存储系统，关注点会变成：

```text
顺序写
随机读
pread/pwrite
mmap
fsync/fdatasync
rename 原子提交
崩溃一致性
page cache 行为
```

---

## 5. 工程封装层次

实际服务端工程里，一般不会在业务代码里到处裸写 `open/read/write/close`，而是分层封装。

### 5.1 第一层：RAII 管理 fd 生命周期

C++ 里最基础的封装是 `UniqueFd` / `ScopedFd`。

核心目标：

```text
对象析构时自动 close fd
避免异常、早退、多分支 return 导致 fd 泄漏
```

典型规则：

```text
禁止拷贝
允许移动
析构自动 close
release 只用于所有权转移
```

简化示例：

```cpp
class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}

    ~UniqueFd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const noexcept {
        return fd_;
    }

    int release() noexcept {
        int old = fd_;
        fd_ = -1;
        return old;
    }

    void reset(int new_fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = new_fd;
    }

private:
    int fd_;
};
```

它的语义类似 `std::unique_ptr`，只不过管理的是内核 fd 资源。

---

### 5.2 第二层：封装可靠读写

RAII 只解决资源释放，不解决读写语义。

`read/write` 不保证一次完成，所以工程中经常封装：

```text
readn
writen
write_all
```

例如：

```cpp
ssize_t write_all(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t left = len;

    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n > 0) {
            p += n;
            left -= n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }

    return static_cast<ssize_t>(len);
}
```

如果是非阻塞 fd，还要额外处理：

```text
EAGAIN
EWOULDBLOCK
```

这时不是简单循环死等，而是交回事件循环，等待下一次可写事件。

---

### 5.3 第三层：业务对象封装

真正工程中更常见的是业务级对象：

```text
Logger
ConfigLoader
AtomicFileWriter
PidFile
StaticFileHandler
TempFile
FileCache
```

业务代码不应该关心底层细节：

```cpp
AtomicFileWriter writer("config.json");
writer.write(data);
writer.commit();
```

底层再去处理：

```text
open tmp
write_all
fsync
close
rename
```

---

## 6. RAII 最佳实践

### 6.1 明确所有权

一定要区分：

```text
拥有 fd
借用 fd
```

拥有 fd 的对象负责关闭：

```cpp
UniqueFd fd(open(...));
```

借用 fd 的函数绝不能关闭：

```cpp
void handle_connection(int fd); // borrow only
```

建议在工程里形成约定：

```text
谁创建，谁拥有
谁拥有，谁关闭
借用者不能 close
所有权转移必须显式表达
```

### 6.2 禁止拷贝，支持移动

fd 是独占资源。  
如果允许拷贝，会导致 double close。

```text
UniqueFd a(fd);
UniqueFd b = a; // 危险
```

所以必须禁用 copy，支持 move。

### 6.3 析构函数不要抛异常

析构函数里调用 `close` 失败时，通常只能忽略或记录。

关键数据文件不能依赖析构函数完成持久化。  
应该显式调用：

```text
fsync
close
commit
```

换句话说：

```text
RAII 负责释放资源
commit 负责业务语义
```

这两个不要混在一起。

### 6.4 fd 编号复用是大坑

fd 是小整数，会被内核复用。

常见事故链路：

```text
1. 连接 A 的 fd=10
2. A 被 close
3. 新连接 B 复用 fd=10
4. epoll 中还有 A 的旧事件
5. 旧事件误处理到 B 上
```

所以关闭 fd 前后要保证：

```text
从 epoll 中删除
对象生命周期结束
不会再访问旧事件
```

---

## 7. 常用系统调用：按工程价值记

### 7.1 必须熟悉

```text
open / read / write / close
lseek / pread / pwrite
dup / dup2 / dup3
fcntl
stat / lstat / fstat
opendir / readdir / closedir
mkdir / unlink / rename
chmod / chown / umask
```

重点不是背参数，而是知道：

```text
解决什么问题
操作 fd 还是路径
有什么原子性
失败时怎么看 errno
有什么服务端常见坑
```

### 7.2 用时查，但要知道用途

```text
link / symlink / readlink
truncate / ftruncate
access / faccessat
chdir / getcwd / rmdir
utime / utimes / futimens
```

### 7.3 暂时了解即可

```text
设备文件 major/minor
setuid / setgid / sticky bit
不同 Unix 版本的细节差异
```

---

## 8. 关键 flag 与工程语义

### 8.1 O_CLOEXEC

用于防止 fd 在 `exec` 后泄漏到子进程。

服务端如果会拉起外部命令，应该非常重视这个 flag。

最佳实践：

```text
优先在 open/socket 创建时直接设置 O_CLOEXEC
不要先创建再 fcntl 设置，否则多线程场景可能有竞态
```

### 8.2 O_NONBLOCK

网络服务核心 flag。

它和 epoll 配合使用：

```text
read 返回 EAGAIN：当前没有更多数据
write 返回 EAGAIN：当前写缓冲区满
```

这不是致命错误，而是应该等待下一次事件。

### 8.3 O_APPEND

日志追加常用。

它让内核在每次写入前自动移动到文件末尾，避免多个进程各自 `lseek(SEEK_END)` 带来的竞态。

但仍要注意：

```text
多线程/多进程写日志时，单条日志是否可能交错
日志轮转后是否 reopen
```

### 8.4 O_CREAT | O_EXCL

原子创建文件。

常见用途：

```text
避免覆盖已有文件
简单 lock file
临时文件创建
```

### 8.5 O_TRUNC

打开文件时直接清空内容。

这是危险 flag。

配置文件、元数据文件、索引文件不要直接：

```text
O_TRUNC 写正式文件
```

否则服务崩溃时可能留下空文件或半文件。  
更推荐：

```text
tmp + fsync + rename
```

---

## 9. 可靠读写注意事项

### 9.1 read 的返回值

```text
> 0：实际读到的字节数，可能小于请求值
= 0：EOF；socket 上通常表示对端关闭
= -1：失败，需要检查 errno
```

### 9.2 write 的返回值

```text
> 0：实际写入字节数，可能小于请求值
= -1 且 errno=EINTR：通常重试
= -1 且 errno=EAGAIN：非阻塞 fd 当前不可写
```

### 9.3 不要假设一次 I/O 完成

服务端里最容易踩的坑：

```cpp
write(fd, buf, len); // 错误：默认以为一次写完
```

可靠做法是：

```text
文件写入：write_all
socket 写入：写多少算多少，剩余数据挂到 output buffer，等待下一次 EPOLLOUT
```

这也是网络编程中处理半包、粘包、发送缓冲区的基础。

---

## 10. 原子性与崩溃一致性

### 10.1 rename 原子替换

同一个文件系统内，`rename` 通常具有原子替换语义。

适合：

```text
配置文件更新
元数据提交
snapshot 提交
索引版本切换
```

但注意：

```text
跨文件系统 rename 可能失败，errno=EXDEV
```

### 10.2 安全写文件流程

更严谨的安全写文件流程：

```text
1. open path.tmp
2. write_all(data)
3. fsync(tmp_fd)
4. close(tmp_fd)
5. rename(path.tmp, path)
6. 需要更强保证时 fsync(parent_dir_fd)
```

为什么最后还要考虑 `fsync` 父目录？

因为 `rename` 修改的是目录项。  
如果极端崩溃场景下要保证目录项也持久化，就需要同步父目录。

### 10.3 fsync / fdatasync

```text
write 成功：数据进入内核 page cache
fsync 成功：尽量保证数据和元数据落盘
fdatasync 成功：通常只保证数据和必要元数据落盘
```

工程判断：

```text
普通日志：不一定每条 fsync
审计日志 / WAL / 元数据：需要明确 fsync 策略
```

---

## 11. 文件与目录语义

### 11.1 stat / lstat / fstat

```text
stat：跟随符号链接，查看目标文件
lstat：不跟随符号链接，查看链接本身
fstat：基于 fd 查看文件，减少路径被替换带来的竞态
```

服务端中，如果已经打开文件，优先使用 `fstat` 获取元数据。

### 11.2 硬链接与软链接

硬链接：

```text
多个目录项指向同一个 inode
删除其中一个名字，文件不一定消失
不能跨文件系统
通常不能链接目录
```

软链接：

```text
一个特殊文件，内容是目标路径
可以跨文件系统
目标删除后会变成悬空链接
```

部署系统里常见：

```text
current -> release_xxx
```

就是软链接切版本。

### 11.3 权限与 umask

常见权限：

```text
普通文件：0644
可执行文件/目录：0755
私有文件：0600
私有目录：0700
```

最终权限：

```text
final_mode = mode & ~umask
```

所以：

```cpp
open("a.log", O_CREAT | O_WRONLY, 0644);
```

最终不一定就是 `0644`，还要受 `umask` 影响。

排查 `Permission denied` 时，不只看文件权限，还要看：

```text
父目录是否有 x 权限
用户/组是否正确
umask 是否影响创建权限
SELinux/AppArmor 是否限制
```

---

## 12. 常见 errno 与线上排障

### 12.1 EINTR

系统调用被信号中断。

通常做法：

```text
重试
```

尤其是阻塞式 I/O 中要注意。

### 12.2 EAGAIN / EWOULDBLOCK

非阻塞 I/O 当前不可读或不可写。

这不是致命错误。

正确处理方式：

```text
返回事件循环
等待 epoll 下一次事件
```

### 12.3 EMFILE / ENFILE

```text
EMFILE：当前进程 fd 数量达到上限
ENFILE：系统级 fd 资源达到上限
```

排查方式：

```bash
ulimit -n
ls /proc/<pid>/fd | wc -l
lsof -p <pid>
cat /proc/sys/fs/file-nr
```

服务端出现 `accept: Too many open files`，优先怀疑 fd 泄漏或连接数过高。

### 12.4 ENOSPC

磁盘满。

常见影响：

```text
日志写失败
缓存写失败
数据库写失败
临时文件创建失败
```

注意：磁盘满不一定是可见文件导致，也可能是 deleted 文件仍被进程持有。

### 12.5 EACCES / EPERM

权限不足或操作被策略禁止。

排查：

```text
文件权限
目录权限
运行用户
所属组
SELinux/AppArmor
容器挂载权限
```

### 12.6 ENOENT / EEXIST / EXDEV

```text
ENOENT：路径不存在
EEXIST：O_EXCL 原子创建时目标已存在
EXDEV：跨文件系统 rename 失败
```

---

## 13. 服务端实践规则清单

### 13.1 资源管理

```text
fd 必须有明确所有权
优先使用 RAII 封装 fd
禁止 fd 对象拷贝
移动后源对象置为无效
release 只用于明确的所有权转移
```

### 13.2 I/O 语义

```text
所有系统调用都要检查返回值
read/write 不保证一次完成
阻塞 I/O 要处理 EINTR
非阻塞 I/O 要处理 EAGAIN/EWOULDBLOCK
socket 写不完的数据要进入 output buffer
```

### 13.3 文件更新

```text
不要直接 O_TRUNC 写正式配置/元数据文件
优先 tmp + fsync + rename
write 成功不等于持久化成功
需要崩溃一致性时明确 fsync 策略
```

### 13.4 网络服务

```text
网络 fd 默认考虑 O_NONBLOCK
跨 exec 的 fd 默认考虑 O_CLOEXEC
epoll 注册/注销必须和 fd 生命周期一致
close 前后要防止旧事件误用新 fd
```

### 13.5 日志系统

```text
追加日志使用 O_APPEND
日志轮转后要 reopen
rm 日志不等于释放空间
磁盘满要有降级或告警
关键日志考虑 fsync 策略
```

### 13.6 目录与权限

```text
目录遍历要跳过 . 和 ..
递归目录要防止软链接循环
创建文件/目录要考虑 umask
Permission denied 要同时排查文件和父目录权限
```

---

## 14. C++ 标准库 I/O 与 Unix I/O 的取舍

### 14.1 可以用 fstream 的场景

```text
小配置文件
测试工具
离线脚本
简单文本读写
非性能敏感路径
不需要控制 fd flag 的业务代码
```

例如：

```cpp
std::ifstream in("config.json");
```

这种场景用标准库更简单。

### 14.2 应该用 Unix I/O 的场景

```text
非阻塞 I/O
epoll/socket/pipe/eventfd/timerfd
sendfile
mmap
fsync/fdatasync
fcntl 文件锁
O_CLOEXEC/O_APPEND/O_EXCL/O_NONBLOCK
高性能日志
静态文件服务
原子文件更新
```

也就是说：

```text
普通业务文件：可以标准库
系统编程核心路径：必须理解 Unix fd 语义
```

### 14.3 工程结论

标准库封装提升开发效率，但不能替代底层模型。

服务端工程师至少要能理解：

```text
fd 生命周期
非阻塞 I/O
部分读写
文件原子更新
权限和 umask
unlink/rename/inode 语义
```

---

## 15. 建议练习项目

相比反复背 API，更推荐用小项目把模型串起来。

### 15.1 mycat

目标：

```bash
mycat file.txt
```

练习：

```text
open
read
write
close
EINTR
部分写
```

### 15.2 mycp

目标：

```bash
mycp a.txt b.txt
```

练习：

```text
fstat 获取源文件权限
open 创建目标文件
read/write 循环
错误处理
```

### 15.3 myls -l

目标：

```bash
myls -l .
```

练习：

```text
opendir
readdir
lstat
st_mode
权限位解析
文件类型判断
```

### 15.4 safe_write

目标：

```cpp
safe_write("config.json", data);
```

练习：

```text
tmp 文件
write_all
fsync
rename
崩溃一致性
```

### 15.5 pid_lock

目标：

```text
服务启动时防止重复运行
```

练习：

```text
pid 文件
fcntl 文件锁
进程退出自动释放锁
```

### 15.6 static_file_server

目标：

```text
HTTP 返回静态文件
```

练习：

```text
socket fd
file fd
fstat
sendfile
epoll
```

---

## 16. 最终总结

APUE 第 3、4 章看起来 API 很多，但从服务端工程角度，它们真正想让你建立的是这一套能力：

```text
理解 fd 是服务端资源管理的核心抽象
理解路径、目录项、inode 的关系
理解 read/write 的真实语义
理解 close、dup、fork、exec 对 fd 生命周期的影响
理解 unlink/rename 的文件系统语义
理解权限、umask、软硬链接
理解 O_NONBLOCK、O_CLOEXEC、O_APPEND、O_EXCL 等关键 flag
知道如何用 RAII 管理 fd
知道如何写不会损坏配置、不会泄漏 fd、不会误判 I/O 结果的服务端代码
```

一句话概括：

> Unix 文件 I/O 的工程价值，不是让你背 API，而是让你掌握服务端程序和内核资源交互的基本模型。
