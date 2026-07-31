```md
# MiniReactor Phase2 优化方向

## 目标

Phase1 已经实现：

- epoll事件循环
- Channel事件分发
- TCP连接管理
- 基础Buffer
- Echo Server

Phase2 不改变整体架构，主要优化：

> 提升并发能力、减少阻塞、完善工程化设计。

---

# 1. 多线程 Reactor 模型（核心优化）

## Phase1问题

当前：

```

一个线程

EventLoop
|
所有连接

```

问题：

- 单核利用率有限
- 大量连接时事件处理压力集中


---

## 优化

采用：

## One Loop Per Thread


架构：

```

```
         Main Reactor

             |
             |
    EventLoopThreadPool

   /          |          \
```

WorkerLoop  WorkerLoop  WorkerLoop

```
   |
   |
```

TcpConnection

```


职责：

### Main Reactor

负责：

- accept新连接


### Worker Reactor

负责：

- IO事件
- connection读写


---

# 2. EventLoopThreadPool


新增模块：

```

EventLoopThreadPool

```
|
|
```

EventLoopThread

```
|
|
```

EventLoop

```


功能：

- 创建多个IO线程
- 管理多个EventLoop
- 新连接负载均衡


分配策略：

第一版：

```

round robin

```


例如：

```

conn1 -> loop1

conn2 -> loop2

conn3 -> loop3

```


---

# 3. 非阻塞写优化


## Phase1问题


write流程：

```

send()

|
|
write(fd)

```


如果socket发送缓冲区满：

```

write阻塞

```


影响EventLoop。


---

## 优化


增加OutputBuffer：

```

TcpConnection

InputBuffer

OutputBuffer

```


流程：

```

send()

|
|
append outputBuffer

|
|
注册EPOLLOUT

|
|
socket可写

|
|
write()

```


---

# 4. Buffer优化


## Phase1

可能：

```

std::vector<char>

```


问题：

- 频繁扩容
- 数据移动


---

## 优化


采用：

## prependable buffer


结构：

```

+---------+----------+---------+

prepend readable writable

```


优势：

- 减少移动
- 支持协议解析


---

# 5. Epoll优化


## LT → ET


Phase1：

Level Trigger


问题：

事件重复通知。


优化：

Edge Trigger


```

EPOLLET

```


要求：

必须：

```

while(read)

直到EAGAIN

```


否则丢事件。


---

# 6. Channel优化


## Phase1

事件处理简单：

```

handleEvent()

```


优化：

增加状态：

```

New

Added

Deleted

```


管理：

- 是否注册epoll
- 当前关注事件


新增：

```

enableReading()

disableReading()

enableWriting()

disableWriting()

```


类似muduo设计。


---

# 7. Connection生命周期优化


新增状态机：

```

Connecting

Connected

Disconnecting

Disconnected

```


解决：

- 重复关闭
- 异步关闭
- 半关闭


---

# 8. 定时器接入


复用之前实现的：

TimerScheduler


应用：

## 连接超时


例如：

```

连接建立

```
|
|
```

5分钟无数据

```
|
|
```

close

```


## 心跳检测


```

heartbeat timeout

```
    |

  close
```

```


---

# 9. 异步任务投递


问题：

其他线程不能直接操作EventLoop。


例如：

业务线程：

```

connection->send()

```


可能跨线程。


---

增加：

```

EventLoop::runInLoop()

```


流程：

```

worker thread

```
  |
  |
```

queue task

```
  |
  |
```

EventLoop线程执行

```


类似：

muduo：

```

queueInLoop()

```


---

# 10. 日志和监控


接入已有：

AsyncLogger


增加：

统计：

```

connection count

read bytes

write bytes

event count

```


方便调试。


---

# Phase2推荐实现顺序


```

1. EventLoopThread

   ```
    ↓
   ```

2. EventLoopThreadPool

   ```
    ↓
   ```

3. TcpConnection跨线程管理

   ```
    ↓
   ```

4. OutputBuffer

   ```
    ↓
   ```

5. runInLoop任务队列

   ```
    ↓
   ```

6. Buffer优化

   ```
    ↓
   ```

7. ET模式

   ```
    ↓
   ```

8. Timer接入

   ```
    ↓
   ```

9. Metrics统计

```


---

# Phase2完成后的架构


```

```
              Acceptor

                 |

          Main EventLoop

                 |

    EventLoopThreadPool


   /          |           \
```

EventLoop     EventLoop    EventLoop

```
   |

   |
```

TcpConnection

```
   |
```

InputBuffer / OutputBuffer

```
   |
```

Socket

```


---

# Phase2核心学习价值


完成后掌握：

- Reactor多线程模型
- IO线程和业务线程分离
- 非阻塞网络编程
- epoll ET模式
- 跨线程任务调度
- TCP连接生命周期管理


这一步完成后，MiniReactor基本达到 **mini-muduo水平**，具备阅读 Redis / brpc 网络层代码的能力。
```
