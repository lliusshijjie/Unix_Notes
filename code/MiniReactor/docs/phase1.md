我建议 MiniReactor 第一阶段不要做成“mini muduo”，而是实现一个**最小可运行 Reactor TCP Server**。

目标：

> 能启动 TCP 服务，接受多个客户端连接，接收数据，并通过回调处理。

这一阶段重点学习：

* epoll 工作机制
* Reactor 事件循环
* fd 与事件绑定
* callback 分发
* TCP连接生命周期

不考虑：

* 多线程 Reactor
* 连接池
* 定时器
* 高性能 Buffer
* 零拷贝
* 协程

下面是 phase1.md。

---

# MiniReactor Phase1

## 目标

实现一个基于 Linux epoll 的最小 Reactor 网络框架。

最终支持：

```text
Client

  |
  |
 TCP

  |
  |

MiniReactor

  |
  |
 EventLoop

  |
  |
 callback

  |
  |
业务处理

```

完成一个 EchoServer：

```
client:

hello


server:

hello
```

---

# 1. 本阶段实现范围

## 必须实现

### TCP Server

支持：

* 创建监听socket
* bind
* listen
* accept

---

### epoll事件管理

支持：

* epoll_create
* epoll_ctl
* epoll_wait

监听：

```
EPOLLIN
EPOLLOUT
EPOLLERR
EPOLLHUP
```

---

### Reactor核心

实现：

```
EventLoop
```

负责：

```
while(running)

    epoll_wait()

    dispatch event

```

---

### Channel

封装：

```
fd

events

callback
```

实现：

```
fd发生事件

↓

找到Channel

↓

执行callback

```

---

### TCP连接管理

支持：

* 建立连接
* 接收数据
* 关闭连接

---

### Buffer

简单实现：

```
InputBuffer
```

支持：

```cpp
append()

readableBytes()

retrieve()

```

用于保存TCP接收数据。

---

# 2. 整体架构

```
                 Client

                   |
                   |
                   v


             listen socket


                   |
                   |

              Acceptor


                   |
                   |

             new connection


                   |
                   |

              TcpConnection


                   |
                   |

              Channel


                   |
                   |

              EventLoop


                   |
                   |

              EpollPoller



```

---

# 3. 目录结构

```
MiniReactor

├── CMakeLists.txt

├── include

│
├── src
│
│
├── base
│   │
│   └── NonCopyable.h
│
│
├── net
│
│   ├── EventLoop.h
│   ├── EventLoop.cpp
│   │
│   ├── Poller.h
│   ├── EpollPoller.h
│   ├── EpollPoller.cpp
│   │
│   ├── Channel.h
│   ├── Channel.cpp
│   │
│   ├── Socket.h
│   ├── Socket.cpp
│   │
│   ├── Acceptor.h
│   ├── Acceptor.cpp
│   │
│   ├── TcpConnection.h
│   ├── TcpConnection.cpp
│   │
│   ├── TcpServer.h
│   ├── TcpServer.cpp
│   │
│   ├── Buffer.h
│   └── Buffer.cpp
│
│
├── example
│
│   └── EchoServer.cpp
│
└── test

```

---

# 4. 模块说明

## 4.1 EventLoop

核心模块。

职责：

维护事件循环。

接口：

```cpp
class EventLoop {

public:

    void loop();

    void quit();


};
```

核心逻辑：

```cpp
while(!quit_)
{

    auto events =
        poller_->poll();


    for(auto channel : events)
    {
        channel->handleEvent();
    }

}

```

---

## 4.2 Poller

封装IO多路复用。

抽象：

```cpp
class Poller
{

public:

virtual std::vector<Channel*>
poll(int timeout)=0;


virtual void updateChannel(
Channel*)=0;


virtual void removeChannel(
Channel*)=0;

};

```

实现：

```
Poller

   |
   |
EpollPoller

```

---

## 4.3 EpollPoller

负责：

```
epoll_create

epoll_ctl

epoll_wait

```

维护：

```cpp
std::unordered_map<int, Channel*>
```

作用：

```
fd

↓

Channel
```

---

## 4.4 Channel

表示：

```
一个fd对应的事件
```

结构：

```cpp
class Channel
{

int fd_;

uint32_t events_;


ReadCallback readCallback_;

WriteCallback writeCallback_;


};
```

例如：

socket：

```
fd=10

events:

EPOLLIN

```

发生：

```
readable

↓

handleEvent()

↓

readCallback()
```

---

## 4.5 Socket

简单封装：

Linux socket API。

提供：

```cpp
create();

bind();

listen();

accept();

close();

```

负责：

```
fd资源管理
```

使用RAII：

```cpp
class Socket
{

int fd_;

~Socket()
{
    close(fd_);
}

};

```

---

## 4.6 Acceptor

负责：

监听新连接。

流程：

```
listen fd

       |
       |
epoll readable


       |

accept()


       |

TcpConnection

```

接口：

```cpp
class Acceptor
{

void listen();

void setNewConnectionCallback();

};

```

---

## 4.7 TcpConnection

表示：

一个客户端连接。

负责：

* socket fd
* 读取数据
* 写数据
* 关闭

状态：

```cpp
enum State
{

Connected,

Disconnected

};

```

接口：

```cpp
send();

shutdown();

handleRead();

```

---

## 4.8 Buffer

解决TCP拆包问题。

例如：

客户端发送：

```
hello world
```

可能：

第一次：

```
hello
```

第二次：

```
 world
```

所以：

保存：

```
input buffer


+----------------+
|hello world     |
+----------------+

```

接口：

```cpp
append();

retrieve();

readableBytes();

```

---

# 5. 数据流程

## 新连接

```
client connect


↓

listen fd EPOLLIN


↓

Acceptor


↓

accept()


↓

创建TcpConnection


↓

注册Channel


```

---

## 收数据

```
client send


↓

socket fd readable


↓

epoll_wait


↓

Channel


↓

TcpConnection::handleRead


↓

Buffer


↓

message callback


```

---

## Echo

业务：

```cpp
connection->send(buffer);
```

流程：

```
Buffer

↓

socket write

↓

client

```

---

# 6. 第一阶段不实现

以下全部延后：

## 多线程EventLoop

当前：

```
一个线程

EventLoop

```

以后：

```
main loop

      |

EventLoopThreadPool

      |

worker loops

```

---

## 定时器

暂不接入：

TimerScheduler

---

## ThreadPool

暂不接入：

业务线程池。

---

## 高性能Buffer

暂时：

std::vector<char>

以后：

* prepend buffer
* zero copy
* scatter/gather

---

## 高性能优化

暂不考虑：

* edge trigger
* busy polling
* io_uring
* splice
* sendfile

---

# 7. 验收标准

完成后：

## Server启动

输出：

```
server listening 8080
```

---

## 多客户端连接

支持：

```
client1

client2

client3

```

同时连接。

---

## Echo测试

客户端：

```
hello
```

服务端：

```
hello
```

---

## 基本关闭

客户端关闭：

```
close connection
```

服务端：

```
remove Channel

close fd

```

---

# 8. 推荐实现顺序

严格按照：

```
1. Socket

2. Channel

3. EpollPoller

4. EventLoop

5. Acceptor

6. Buffer

7. TcpConnection

8. TcpServer

9. EchoServer

```

---

# 本阶段学习目标

完成 Phase1 后，需要掌握：

1. Linux epoll模型

```
fd
 |
epoll
 |
event
 |
callback

```

2. Reactor模式

```
等待事件

↓

分发事件

↓

执行回调

```

3. TCP服务器基本生命周期

```
socket

↓

bind

↓

listen

↓

accept

↓

read/write

↓

close

```

4. 网络框架基本分层

```
EventLoop

    |
Poller

    |
Channel

    |
Connection

    |
Socket

```

---

# Phase1完成标准

* [ ] epoll封装完成
* [ ] EventLoop运行正常
* [ ] Channel事件分发正常
* [ ] Acceptor接受连接
* [ ] TcpConnection管理连接
* [ ] Buffer保存数据
* [ ] EchoServer运行成功
* [ ] 支持多个客户端连接

---

这一阶段的核心不是性能，而是**理解 Reactor 网络服务器的骨架**。

完成 Phase1 后，你就具备阅读：

* muduo Reactor
* Redis ae event loop
* brpc bthread/socket层

的基础。
