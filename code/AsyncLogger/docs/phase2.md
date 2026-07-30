已完成第一版异步日志库后，第二版优化的目标应该从**“正确运行”转向“服务端高性能场景适用”**。

第一版的问题本质：

```text
业务线程
  |
  | log()
  ↓
mutex竞争
  ↓
std::deque
  ↓
后台线程
  ↓
ofstream
```

虽然架构正确，但是高并发下存在几个瓶颈：

1. 多线程竞争同一把 mutex。
2. 每条日志都可能产生大量内存分配。
3. 日志线程批量能力不足。
4. 文件 I/O 效率一般。
5. 时间、格式化等重复计算。

第二版优化应该围绕这些方向展开。

---

# 异步日志库第二版优化方向

## 1. 队列模型优化（最高优先级）

### 第一版

```cpp
std::deque<LogRecord>
+
mutex
+
condition_variable
```

问题：

多个业务线程：

```
Thread A
    |
Thread B ---> mutex ---> deque
    |
Thread C
```

所有生产者竞争同一个锁。

---

## 优化方向1：无锁/低锁队列

引入：

* SPSC Queue
* MPSC Queue
* MPMC Queue

对于日志系统：

天然模型：

```
业务线程 A
业务线程 B
业务线程 C
       |
       |
       v
    日志队列
       |
       v
   一个worker
```

这是：

```
MPSC
Multiple Producer
Single Consumer
```

所以没有必要使用 MPMC。

---

优化：

```text
多个生产者

    ↓

MPSC Ring Buffer

    ↓

日志线程
```

优势：

* 去除生产者锁竞争。
* 固定容量。
* cache friendly。
* 内存连续。

例如：

```cpp
std::array<LogRecord, 8192>
```

代替：

```cpp
std::deque
```

---

# 2. LogRecord 内存优化

第一版：

```cpp
struct LogRecord {

    std::string message;

};
```

问题：

每条日志：

```
创建string
    |
malloc
    |
copy
    |
释放
```

高频日志：

```
100万条/s

= 百万次malloc
```

非常浪费。

---

优化方向：

## 固定大小buffer

例如：

```cpp
struct LogMessage {

    char data[512];

};
```

流程：

```
格式化

↓

stack buffer

↓

queue

↓

write
```

减少：

* malloc
* free
* cache miss

---

# 3. 双缓冲日志模型

这是工业日志库非常经典的优化。

第一版：

```
worker:

取一条

写一条

取一条

写一条
```

问题：

大量小写。

---

优化：

两个buffer：

```
前台线程

buffer A
    |
    |
swap

buffer B


后台线程

批量写buffer A
```

流程：

```
业务线程

current buffer

        |
        | swap
        ↓


后台线程

old buffer

        |
        |
     write()
```

优势：

* 减少锁时间。
* 批量写。
* 提高吞吐。

典型：

Google 的 glog、Apache Software Foundation 生态很多日志系统都有类似思想。

---

# 4. 批量写入优化

第一版：

```
log1
write()

log2
write()

log3
write()
```

系统调用次数：

```
N条日志

=N次write
```

优化：

```
日志1
日志2
日志3

↓

buffer

↓

一次write()
```

甚至：

```cpp
writev()
```

一次写多个buffer。

收益：

减少：

* 用户态/内核态切换
* syscall次数

---

# 5. 时间戳优化

第一版：

每条：

```
clock_gettime()

strftime()

格式化日期
```

问题：

时间获取也很贵。

优化：

后台维护：

```
cached_time
```

例如：

```
2026-07-30 21:00:00
```

这一秒内：

只更新：

```
.microseconds
```

变成：

```
一次秒级转换

百万次复用
```

---

# 6. 日志格式化优化

第一版：

可能：

```cpp
std::string +
std::to_string
```

问题：

产生大量临时对象。

优化：

使用：

## snprintf

或者：

## fmt库

例如：

```cpp
fmt::format_to(
    buffer,
    "{} {}",
    level,
    message
);
```

进一步：

compile-time format：

```cpp
FMT_COMPILE()
```

减少运行期开销。

---

# 7. 线程模型优化

第一版：

```
一个worker
```

一般够用。

但是：

大量日志：

```
10GB/min
```

一个线程可能不足。

优化：

日志写线程池：

```
worker1
worker2
worker3

      |
      |

   file writer
```

不过注意：

日志写入天然有序。

所以：

普通服务：

```
一个worker
+
批量写
```

通常已经足够。

---

# 8. 日志等级编译期过滤

第一版：

运行判断：

```cpp
if(level < min_level)
```

优化：

宏：

```cpp
#ifndef DEBUG
#define LOG_DEBUG(...)
#endif
```

例如：

Release:

```cpp
LOG_TRACE()
```

直接消失。

减少：

* 字符串构造
* 函数调用

---

# 9. 文件系统优化

第一版：

```cpp
ofstream
```

可以优化：

## fwrite

或者：

## Linux write

减少：

iostream层开销。

进一步：

## mmap

流程：

```
日志

↓

memory map

↓

OS负责刷盘
```

适合：

大量日志。

---

# 10. 崩溃安全优化

生产环境：

日志不能全部丢。

增加：

## fsync策略

例如：

普通：

```
5s flush
```

错误：

```
立即flush
```

Fatal：

```
flush + fsync
```

---

# 第二版推荐实现优先级

不要全部做。

按照服务端价值排序：

| 优先级   | 优化               | 推荐    |
| ----- | ---------------- | ----- |
| ⭐⭐⭐⭐⭐ | MPSC RingBuffer  | 必做    |
| ⭐⭐⭐⭐⭐ | 双缓冲              | 必做    |
| ⭐⭐⭐⭐  | 批量写入             | 必做    |
| ⭐⭐⭐⭐  | 固定Buffer减少malloc | 必做    |
| ⭐⭐⭐   | 时间缓存             | 推荐    |
| ⭐⭐⭐   | fmt格式化           | 推荐    |
| ⭐⭐    | 编译期日志过滤          | 推荐    |
| ⭐⭐    | writev           | 可选    |
| ⭐     | mmap             | 高级    |
| ⭐     | 多worker          | 一般不需要 |

---

# 第二版最终目标架构

```
                业务线程

        Thread A Thread B Thread C

              |
              |
              v

        MPSC RingBuffer

              |
              |
              v


        Async Logger Worker


              |
              |
        双Buffer交换


              |
              |

       批量格式化


              |
              |

       write/writev


              |
              |

          日志文件
```

---

# 第二版学习价值

完成第二版后，你会真正理解工业日志库里面几个核心思想：

* 为什么服务端喜欢 RingBuffer。
* 为什么很多高性能组件避免 malloc。
* 为什么批量处理比单次处理快。
* 为什么锁竞争是高并发系统瓶颈。
* 为什么 Redis、Kafka、Nginx 都大量使用 buffer 思想。

这版优化完成后，异步日志库基本就达到 **工程级 C++ 服务端组件水平**。
