# 有界阻塞队列 Phase 2：非阻塞、超时与完整接口

## 1. 阶段定位

第一阶段已经解决了有界阻塞队列最核心的问题：

* 多生产者、多消费者并发安全；
* 队列满时阻塞生产者；
* 队列空时阻塞消费者；
* 支持 `close()`；
* 关闭后排空剩余元素；
* 支持 move-only 类型；
* 具备清晰的生命周期和退出语义。

第二阶段暂时不修改底层存储结构，仍然使用：

```cpp
std::deque<T>
std::mutex
std::condition_variable
```

本阶段的重点是完善接口，使队列能够适应真实服务端中的不同等待策略：

```text
无限等待
非阻塞尝试
有限时间等待
原地构造
明确失败原因
```

完成第二阶段后，队列应从一个基础并发练习，演进为可以复用于线程池、异步日志、任务流水线和网络任务投递的通用组件。

---

# 2. 第二阶段目标

本阶段建议完成以下五项能力：

1. 实现非阻塞入队和出队；
2. 实现带超时的入队和出队；
3. 实现原地构造 `emplace`；
4. 完善操作结果的错误表达；
5. 补充完整的边界测试和并发测试。

推荐新增接口：

```cpp
template <typename T>
class BlockingQueue {
public:
    bool try_push(const T& value);
    bool try_push(T&& value);

    bool try_pop(T& value);

    template <typename... Args>
    bool emplace(Args&&... args);

    template <typename Rep, typename Period>
    bool push_for(
        const T& value,
        const std::chrono::duration<Rep, Period>& timeout);

    template <typename Rep, typename Period>
    bool push_for(
        T&& value,
        const std::chrono::duration<Rep, Period>& timeout);

    template <typename Rep, typename Period>
    bool pop_for(
        T& value,
        const std::chrono::duration<Rep, Period>& timeout);
};
```

如果希望进一步提高接口表达能力，可以引入状态枚举：

```cpp
enum class QueueStatus {
    success,
    closed,
    full,
    empty,
    timeout
};
```

最终接口可以返回 `QueueStatus`，而不是单纯返回 `bool`。

---

# 3. 本阶段开发范围

## 3.1 必须实现

第二阶段必须实现：

* `try_push(const T&)`
* `try_push(T&&)`
* `try_pop(T&)`
* `push_for(const T&, timeout)`
* `push_for(T&&, timeout)`
* `pop_for(T&, timeout)`
* 阻塞式 `emplace`
* 超时使用单调时钟
* 正确处理虚假唤醒
* 正确处理关闭与超时竞争
* 操作失败时不破坏输入、输出对象
* 完整单元测试
* 多线程关闭和超时测试
* ThreadSanitizer 检查

## 3.2 推荐实现

推荐增加：

* `try_emplace`
* `emplace_for`
* `QueueStatus`
* `max_size()` 或保留 `capacity()`
* `clear()` 是否提供的设计分析
* 基础统计信息

## 3.3 暂不实现

本阶段仍然不实现：

* 固定数组环形缓冲区；
* 原始内存管理；
* placement new；
* 批量入队和批量出队；
* 无锁队列；
* 公平等待；
* 自旋优化；
* 信号量；
* 动态扩容；
* 优先级；
* 水位控制；
* 自适应背压策略。

这些内容应留到后续阶段，避免接口完善和底层存储优化同时进行。

---

# 4. 推荐完整接口

第二阶段完成后的接口可以设计为：

```cpp
template <typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(std::size_t capacity);

    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;
    BlockingQueue(BlockingQueue&&) = delete;
    BlockingQueue& operator=(BlockingQueue&&) = delete;

    bool push(const T& value);
    bool push(T&& value);

    template <typename... Args>
    bool emplace(Args&&... args);

    bool try_push(const T& value);
    bool try_push(T&& value);

    template <typename... Args>
    bool try_emplace(Args&&... args);

    template <typename Rep, typename Period>
    bool push_for(
        const T& value,
        const std::chrono::duration<Rep, Period>& timeout);

    template <typename Rep, typename Period>
    bool push_for(
        T&& value,
        const std::chrono::duration<Rep, Period>& timeout);

    template <typename Rep, typename Period, typename... Args>
    bool emplace_for(
        const std::chrono::duration<Rep, Period>& timeout,
        Args&&... args);

    bool pop(T& value);
    bool try_pop(T& value);

    template <typename Rep, typename Period>
    bool pop_for(
        T& value,
        const std::chrono::duration<Rep, Period>& timeout);

    void close();

    bool closed() const;
    bool empty() const;
    bool full() const;

    std::size_t size() const;
    std::size_t capacity() const noexcept;

private:
    std::size_t capacity_;
    std::deque<T> queue_;

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;

    bool closed_{false};
};
```

接口数量虽然有所增加，但其内部逻辑应尽量复用，避免每个函数独立编写一套并发流程。

---

# 5. 非阻塞接口

## 5.1 `try_push`

接口：

```cpp
bool try_push(const T& value);
bool try_push(T&& value);
```

语义：

* 不等待条件变量；
* 获取互斥锁后立即检查队列状态；
* 队列未关闭且存在空位时入队；
* 队列已满或者已经关闭时立即返回失败。

流程：

```text
获取锁
  ↓
检查 closed_
  ├─ 已关闭：返回 false
  └─ 未关闭：继续
  ↓
检查队列是否已满
  ├─ 已满：返回 false
  └─ 未满：入队
  ↓
释放锁
  ↓
唤醒一个消费者
```

参考逻辑：

```cpp
bool try_push(const T& value)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (closed_ || queue_.size() >= capacity_) {
            return false;
        }

        queue_.push_back(value);
    }

    not_empty_.notify_one();
    return true;
}
```

右值版本只应在确定能够入队后移动：

```cpp
queue_.push_back(std::move(value));
```

失败时不应提前移动调用者的对象。

---

## 5.2 `try_pop`

接口：

```cpp
bool try_pop(T& value);
```

语义：

* 不等待元素；
* 队列非空时立即取出队头；
* 队列为空时立即返回失败；
* 无论队列是否关闭，只要还有元素，都允许取出。

状态表：

| 队列状态 | 是否为空 | 结果   |
| ---- | ---: | ---- |
| 未关闭  |    否 | 成功取出 |
| 未关闭  |    是 | 立即失败 |
| 已关闭  |    否 | 成功取出 |
| 已关闭  |    是 | 立即失败 |

参考逻辑：

```cpp
bool try_pop(T& value)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (queue_.empty()) {
            return false;
        }

        value = std::move(queue_.front());
        queue_.pop_front();
    }

    not_full_.notify_one();
    return true;
}
```

不能因为 `closed_ == true` 就直接失败，因为关闭后仍然需要允许消费者排空队列。

---

# 6. 超时接口

## 6.1 为什么需要超时操作

无限阻塞接口适合后台工作线程：

```cpp
queue.pop(task);
```

但在以下场景中，调用方通常不能无限等待：

* RPC 请求有 deadline；
* 日志线程需要定期刷新；
* 线程池提交任务需要限时；
* 服务关闭阶段需要控制等待时间；
* 监控线程需要周期性检查退出条件；
* 网络事件循环不能长期阻塞。

因此需要提供：

```cpp
push_for(...)
pop_for(...)
```

它们表示：

> 在指定时间内等待条件成立，超过时间则返回失败。

---

## 6.2 `push_for`

接口：

```cpp
template <typename Rep, typename Period>
bool push_for(
    const T& value,
    const std::chrono::duration<Rep, Period>& timeout);
```

以及：

```cpp
template <typename Rep, typename Period>
bool push_for(
    T&& value,
    const std::chrono::duration<Rep, Period>& timeout);
```

行为：

1. 获取互斥锁；
2. 如果队列已满且未关闭，则等待；
3. 队列出现空间时继续；
4. 队列关闭时立即失败；
5. 等待时间耗尽时失败；
6. 成功获得空间后入队；
7. 唤醒一个消费者。

返回 `false` 可能表示：

* 队列已经关闭；
* 等待超时。

这也是后续推荐引入 `QueueStatus` 的原因。

---

## 6.3 `pop_for`

接口：

```cpp
template <typename Rep, typename Period>
bool pop_for(
    T& value,
    const std::chrono::duration<Rep, Period>& timeout);
```

行为：

1. 获取互斥锁；
2. 队列为空且未关闭时等待；
3. 等待期间出现元素则取出；
4. 队列关闭但仍有元素时继续消费；
5. 队列关闭且为空时立即失败；
6. 等待时间耗尽时失败。

需要保证：

> `pop_for` 失败时，不修改输出参数 `value`。

也就是说，只有确定队列非空时，才能执行：

```cpp
value = std::move(queue_.front());
```

---

# 7. 使用 `wait_for` 还是 `wait_until`

两种常见写法分别是：

```cpp
condition.wait_for(lock, timeout, predicate);
```

和：

```cpp
condition.wait_until(lock, deadline, predicate);
```

简单情况下，直接使用带谓词的 `wait_for` 是正确的：

```cpp
bool ready = not_full_.wait_for(lock, timeout, [this] {
    return closed_ || queue_.size() < capacity_;
});
```

但如果自己手写循环，不应在每次虚假唤醒后重新等待完整的 `timeout`：

```cpp
while (!condition) {
    condition.wait_for(lock, timeout);
}
```

这种写法可能使总等待时间不断延长。

更稳健的通用做法是：

```cpp
const auto deadline =
    std::chrono::steady_clock::now() + timeout;
```

然后统一使用：

```cpp
condition.wait_until(lock, deadline, predicate);
```

这样无论发生多少次虚假唤醒，总等待时间都不会超过原始 deadline 太多。

第二阶段建议内部统一转换为绝对截止时间。

---

# 8. 为什么必须使用 `steady_clock`

超时等待应使用：

```cpp
std::chrono::steady_clock
```

不应使用：

```cpp
std::chrono::system_clock
```

原因是系统时间可能被修改：

* NTP 校时；
* 管理员手动修改时间；
* 夏令时调整；
* 虚拟机时钟跳变。

`steady_clock` 保证时间单调前进，更适合计算超时和 deadline。

推荐形式：

```cpp
const auto deadline =
    std::chrono::steady_clock::now() + timeout;
```

后续所有超时接口都基于该 deadline 等待。

---

# 9. 超时与关闭的竞争语义

超时和关闭可能几乎同时发生。

例如：

```text
生产者等待队列空位
    ↓
等待期限即将到达
    ↓
另一个线程调用 close()
```

此时操作可能观察到：

* 先观察到关闭；
* 先观察到超时。

对于 `bool` 返回值而言，两者都返回 `false`，不存在接口冲突。

如果引入 `QueueStatus`，需要制定明确规则。

推荐规则：

1. 被唤醒并持锁后，优先检查 `closed_`；
2. 如果队列已关闭，返回 `QueueStatus::closed`；
3. 如果队列未关闭且条件仍不成立，返回 `QueueStatus::timeout`。

也就是说，在调用返回的那个锁内观察点：

```text
closed 优先于 timeout
```

这种规则更利于服务关闭阶段的错误诊断。

---

# 10. 零超时和负超时

超时参数可能为：

```cpp
0ms
```

甚至是负数。

推荐统一定义：

```text
timeout <= 0
```

等价于一次非阻塞尝试。

也就是说：

```cpp
queue.push_for(value, 0ms);
```

语义应与：

```cpp
queue.try_push(value);
```

接近。

同理：

```cpp
queue.pop_for(value, 0ms);
```

应接近：

```cpp
queue.try_pop(value);
```

但不建议简单地在接口外层直接调用 `try_push`，否则不同重载之间可能产生重复代码。

可以通过统一内部实现完成。

---

# 11. 原地构造 `emplace`

## 11.1 为什么需要 `emplace`

传统写法：

```cpp
Task task(arg1, arg2, arg3);
queue.push(std::move(task));
```

通常需要：

1. 在队列外构造一个临时对象；
2. 再将临时对象移动进队列。

使用 `emplace`：

```cpp
queue.emplace(arg1, arg2, arg3);
```

可以直接在 `std::deque` 尾部构造对象：

```cpp
queue_.emplace_back(std::forward<Args>(args)...);
```

这样能够：

* 避免显式临时对象；
* 支持不可复制类型；
* 更自然地传递构造参数；
* 保留完美转发语义。

---

## 11.2 阻塞式 `emplace`

接口：

```cpp
template <typename... Args>
bool emplace(Args&&... args);
```

流程和阻塞式 `push` 一致：

```text
等待队列出现空间
    ↓
检查队列是否关闭
    ↓
在队列尾部原地构造
    ↓
唤醒消费者
```

核心操作：

```cpp
queue_.emplace_back(std::forward<Args>(args)...);
```

需要包含：

```cpp
#include <utility>
```

---

## 11.3 参数生命周期问题

`emplace` 的参数可能包含引用。

例如：

```cpp
std::string name = "task";
queue.emplace(name);
```

在当前同步调用中没有问题，因为参数在 `emplace` 返回前仍然有效。

但需要注意：

* 参数只在真正构造元素时使用；
* 阻塞等待期间，调用线程仍停留在 `emplace` 中；
* 传入参数必须在整个调用期间有效。

不应将参数保存到队列内部，等待以后再构造。正确做法是等到获得容量后，立即在锁内调用 `emplace_back`。

---

## 11.4 `try_emplace`

推荐实现：

```cpp
template <typename... Args>
bool try_emplace(Args&&... args);
```

语义：

* 队列有空位且未关闭时原地构造；
* 队列满或已关闭时立即失败；
* 失败时不消费、移动构造参数。

但对于右值实参，需要理解一个细节：

```cpp
queue.try_emplace(std::move(value));
```

调用函数本身不会自动移动 `value`。只有执行：

```cpp
std::forward<Args>(args)...
```

并真正调用构造函数时才发生移动。

因此只要在确认可入队后才调用 `emplace_back`，失败路径不会消耗实参。

---

## 11.5 `emplace_for`

推荐增加：

```cpp
template <typename Rep, typename Period, typename... Args>
bool emplace_for(
    const std::chrono::duration<Rep, Period>& timeout,
    Args&&... args);
```

需要注意参数顺序。

推荐将 timeout 放在第一个参数：

```cpp
queue.emplace_for(100ms, arg1, arg2);
```

如果 timeout 放到参数包后面，模板推导和调用可读性都会变差。

---

# 12. 返回值设计

## 12.1 继续使用 `bool`

优点：

* 接口简单；
* 与第一阶段保持一致；
* 调用代码简洁。

例如：

```cpp
if (!queue.push_for(task, 100ms)) {
    handle_failure();
}
```

缺点是无法区分：

* 队列已关闭；
* 队列已满；
* 等待超时；
* 队列为空。

对于基础组件，失败原因往往是重要信息。

---

## 12.2 引入 `QueueStatus`

推荐定义：

```cpp
enum class QueueStatus {
    success,
    closed,
    full,
    empty,
    timeout
};
```

不同接口的返回值范围：

| 接口         | 可能结果                         |
| ---------- | ---------------------------- |
| `push`     | `success`、`closed`           |
| `try_push` | `success`、`closed`、`full`    |
| `push_for` | `success`、`closed`、`timeout` |
| `pop`      | `success`、`closed`           |
| `try_pop`  | `success`、`empty`            |
| `pop_for`  | `success`、`closed`、`timeout` |

注意 `try_pop` 在“已关闭但仍有元素”时仍然成功。

当队列已关闭且为空时，可以返回：

```cpp
QueueStatus::closed
```

而在未关闭但为空时返回：

```cpp
QueueStatus::empty
```

这比 `bool` 更有表达力。

---

## 12.3 推荐演进方案

为了避免第二阶段改动过大，可以保留现有 `bool` 接口，同时内部先统一成状态枚举：

```cpp
QueueStatus push_impl(...);
QueueStatus pop_impl(...);
```

外部 `bool` 接口再转换：

```cpp
return status == QueueStatus::success;
```

后续如果要公开状态接口，可以增加：

```cpp
QueueStatus try_push_status(...);
QueueStatus pop_for_status(...);
```

不过要避免长期保留两套高度重复的公开接口。

如果项目目标是学习接口设计，本阶段可以直接切换到 `QueueStatus`。

如果项目目标是尽快完成可靠实现，则可以继续使用 `bool`，并在 README 中注明失败原因不可区分。

---

# 13. 内部代码复用设计

第二阶段接口增多后，最大的风险不是并发错误，而是代码重复。

例如以下函数逻辑高度相似：

```cpp
push
try_push
push_for
emplace
try_emplace
emplace_for
```

它们的区别主要在于等待策略：

```text
无限等待
不等待
等待到 deadline
```

可以设计内部等待模式：

```cpp
enum class WaitMode {
    blocking,
    non_blocking,
    timed
};
```

也可以设计私有辅助函数：

```cpp
bool wait_not_full(
    std::unique_lock<std::mutex>& lock);

bool wait_not_full_until(
    std::unique_lock<std::mutex>& lock,
    std::chrono::steady_clock::time_point deadline);
```

出队侧类似：

```cpp
bool wait_not_empty(
    std::unique_lock<std::mutex>& lock);

bool wait_not_empty_until(
    std::unique_lock<std::mutex>& lock,
    std::chrono::steady_clock::time_point deadline);
```

但是不建议为了减少几行代码而使用过度复杂的模板元编程。

第二阶段的复用原则应是：

> 消除并发判断逻辑的重复，而不是追求所有接口共用一个极其复杂的万能函数。

---

# 14. 状态不变量

第二阶段必须继续维持第一阶段的不变量。

## 14.1 容量不变量

```cpp
queue_.size() <= capacity_
```

## 14.2 关闭不可逆

```text
open → closed
```

不存在重新打开。

## 14.3 共享状态受锁保护

以下状态必须在持有 `mutex_` 时读取和修改：

```cpp
queue_
closed_
```

## 14.4 失败不修改状态

对于所有失败操作：

* `try_push` 失败时队列不变；
* `push_for` 失败时队列不变；
* `try_pop` 失败时输出参数不变；
* `pop_for` 失败时输出参数不变；
* 关闭后入队失败时，右值参数不应被提前移动。

## 14.5 通知只在状态真正变化后发生

只有成功入队后才通知：

```cpp
not_empty_.notify_one();
```

只有成功出队后才通知：

```cpp
not_full_.notify_one();
```

失败路径不应发送无意义通知。

---

# 15. 异常安全要求

## 15.1 `emplace` 构造异常

以下代码可能抛出：

```cpp
queue_.emplace_back(std::forward<Args>(args)...);
```

如果构造失败：

* 队列大小不应增加；
* 互斥锁由 RAII 自动释放；
* 不应通知消费者；
* 异常向上传递。

因此通知必须放在成功构造之后。

---

## 15.2 超时接口异常

超时只是等待失败，不是异常。

以下情况应通过返回值表达，而不是抛异常：

* 队列满；
* 队列空；
* 等待超时；
* 队列关闭。

异常只用于真正的异常情况，例如：

* 元素复制构造抛出；
* 元素移动构造抛出；
* `emplace` 构造函数抛出；
* 内存分配失败。

---

## 15.3 `pop` 的移动异常

顺序仍然必须是：

```cpp
value = std::move(queue_.front());
queue_.pop_front();
```

如果移动赋值抛出，不能先删除元素。

第二阶段仍然可以接受：

```text
T 最好具备 noexcept 移动赋值
```

后续可以通过返回 `std::optional<T>` 改善接口约束。

---

# 16. 查询接口的正确使用

第二阶段加入非阻塞接口后，调用方不应通过查询接口自行拼装原子操作。

错误方式：

```cpp
if (!queue.full()) {
    queue.try_push(value);
}
```

`full()` 返回后，其他线程可能立即入队，因此 `try_push` 仍然可能失败。

同理：

```cpp
if (!queue.empty()) {
    queue.try_pop(value);
}
```

也不能保证成功。

正确方式是直接调用：

```cpp
queue.try_push(value);
queue.try_pop(value);
```

查询接口只能用于：

* 监控；
* 日志；
* 调试；
* 近似状态展示。

不能用于业务同步正确性。

---

# 17. 第二阶段测试计划

## 17.1 `try_push` 成功测试

队列未满且未关闭时：

```text
try_push(value) → true
size 增加 1
元素可以正常 pop
```

分别测试：

* 左值；
* 右值；
* move-only 类型。

---

## 17.2 `try_push` 满队列失败

容量设为 1：

1. 先写入一个元素；
2. 再调用 `try_push`；
3. 应立即返回失败；
4. 队列大小仍然为 1；
5. 已有元素内容不变。

测试不能出现等待行为。

可以记录调用耗时，但不应依赖极其严格的微秒阈值。

---

## 17.3 `try_push` 关闭失败

1. 关闭队列；
2. 调用左值 `try_push`；
3. 调用右值 `try_push`；
4. 两次均失败；
5. 右值对象未被移动。

可以使用自定义 move-only 类型记录移动次数。

---

## 17.4 `try_pop` 成功测试

队列中存在元素时：

```text
try_pop(value) → true
value 正确
size 减少 1
```

---

## 17.5 `try_pop` 空队列失败

队列为空时：

```text
try_pop(value) → false
```

同时验证原有 `value` 不被修改。

例如：

```cpp
int value = 123;
queue.try_pop(value);
```

失败后仍应满足：

```cpp
value == 123
```

---

## 17.6 `try_pop` 关闭后排空

1. 写入多个元素；
2. 关闭队列；
3. 通过 `try_pop` 取出所有元素；
4. 元素耗尽后失败。

证明关闭状态不会阻止剩余元素消费。

---

## 17.7 `push_for` 正常成功

容量为 1：

1. 队列先填满；
2. 启动线程执行 `push_for(value, 1s)`；
3. 主线程在 deadline 前执行一次 `pop`；
4. 等待线程应成功入队。

---

## 17.8 `push_for` 超时

1. 队列保持满状态；
2. 调用 `push_for(value, 50ms)`；
3. 不释放空间；
4. 操作应返回失败；
5. 队列内容保持不变；
6. 输入右值未被提前移动。

耗时验证应允许调度误差，例如只验证：

```text
没有明显早于 deadline 返回
且没有无限等待
```

不要要求精确等于 50ms。

---

## 17.9 `push_for` 等待期间关闭

1. 队列填满；
2. 生产者调用较长超时的 `push_for`；
3. 主线程调用 `close()`；
4. 生产者应尽快返回失败；
5. 不应等待完整 timeout。

---

## 17.10 `pop_for` 正常成功

1. 创建空队列；
2. 消费者执行 `pop_for(value, 1s)`；
3. 主线程在 deadline 前写入元素；
4. 消费者应成功取得元素。

---

## 17.11 `pop_for` 超时

1. 队列保持为空；
2. 调用 `pop_for(value, 50ms)`；
3. 应返回失败；
4. 输出参数保持原值；
5. 不应永久等待。

---

## 17.12 `pop_for` 等待期间关闭

1. 消费者在空队列上调用长超时 `pop_for`；
2. 主线程关闭队列；
3. 消费者应立即返回失败；
4. 不应继续等待到 timeout。

---

## 17.13 `pop_for` 关闭后仍有数据

1. 写入元素；
2. 调用 `close()`；
3. 调用 `pop_for`；
4. 应立即成功取出已有元素；
5. 排空后下一次调用失败。

---

## 17.14 零超时测试

验证：

```cpp
push_for(value, 0ms)
pop_for(value, 0ms)
```

行为接近：

```cpp
try_push(value)
try_pop(value)
```

同时测试负 duration。

---

## 17.15 `emplace` 测试

定义类型：

```cpp
struct Item {
    Item(int id, std::string name);

    Item(const Item&) = delete;
    Item& operator=(const Item&) = delete;

    Item(Item&&) noexcept = default;
    Item& operator=(Item&&) noexcept = default;
};
```

执行：

```cpp
queue.emplace(10, "task");
```

验证：

* 对象构造成功；
* 字段正确；
* 不需要复制构造；
* 可以正常出队。

---

## 17.16 `emplace` 异常测试

定义一个构造函数可能抛异常的类型：

```cpp
struct ThrowingItem {
    explicit ThrowingItem(bool should_throw);
};
```

验证：

* 构造抛出后队列大小不变；
* 队列仍可继续正常使用；
* 没有错误通知消费者；
* 锁没有泄漏。

---

## 17.17 MPMC 混合接口压力测试

建议同时使用：

* 阻塞 `push`；
* `try_push`；
* `push_for`；
* 阻塞 `pop`；
* `try_pop`；
* `pop_for`。

测试配置示例：

```text
4 个生产者
4 个消费者
每个生产者生成 20,000 个唯一元素
队列容量 64
```

生产者可以采用策略：

```text
优先 try_push
失败后使用 push_for
仍失败则重试或记录
```

消费者可以采用：

```text
优先 pop_for
关闭且排空后退出
```

最终验证：

* 所有成功入队元素都被消费；
* 不重复；
* 不丢失；
* 无死锁；
* 关闭后全部线程退出。

---

# 18. 时间测试注意事项

并发超时测试最容易产生偶现失败。

不应断言：

```text
操作必须恰好等待 50ms
```

线程调度、CI 负载和虚拟机环境都会产生误差。

推荐验证区间：

```text
实际耗时不明显小于指定 timeout
实际耗时不超过一个合理上限
```

例如指定 50ms，可以接受：

```text
40ms ～ 500ms
```

具体阈值应根据测试环境调整。

更重要的是验证：

* 是否提前错误返回；
* 是否最终返回；
* 是否被 `close()` 及时唤醒；
* 是否修改了不应修改的对象。

---

# 19. 常见错误

## 19.1 超时后仍然入队

错误逻辑：

```cpp
not_full_.wait_for(lock, timeout);

if (!closed_) {
    queue_.push_back(value);
}
```

`wait_for` 返回并不代表队列一定有空位。

必须使用谓词等待，或者返回后再次检查：

```cpp
queue_.size() < capacity_
```

---

## 19.2 忽略虚假唤醒

错误：

```cpp
not_empty_.wait_for(lock, timeout);
```

被唤醒不等于有元素。

必须以：

```cpp
closed_ || !queue_.empty()
```

作为最终判断依据。

---

## 19.3 每次唤醒重新等待完整超时

错误：

```cpp
while (queue_.empty()) {
    not_empty_.wait_for(lock, timeout);
}
```

多次虚假唤醒会不断重置等待时间。

应使用同一个 deadline：

```cpp
wait_until(lock, deadline, predicate);
```

---

## 19.4 使用 `system_clock`

超时和 deadline 不应依赖可调整的系统时钟。

统一使用：

```cpp
std::chrono::steady_clock
```

---

## 19.5 失败时提前移动参数

错误：

```cpp
T temp = std::move(value);

if (!wait_for_space()) {
    return false;
}
```

等待超时或关闭后，输入对象已经被破坏。

必须在确认能够入队后才执行移动。

---

## 19.6 `try_pop` 看到关闭直接失败

错误：

```cpp
if (closed_) {
    return false;
}
```

关闭后队列里可能仍有元素。

正确判断应优先检查：

```cpp
queue_.empty()
```

---

## 19.7 失败时修改输出参数

错误：

```cpp
value = T{};

if (queue_.empty()) {
    return false;
}
```

接口失败时应尽量保持输出参数不变。

---

## 19.8 通知发送过早

错误：

```cpp
not_empty_.notify_one();
queue_.push_back(value);
```

通知必须发生在共享状态修改完成之后。

正确顺序：

```text
锁内修改状态
释放锁
发送通知
```

---

# 20. 推荐开发顺序

## Step 1：实现 `try_push` 和 `try_pop`

先增加非阻塞接口。

这一部分不涉及条件变量等待，逻辑最简单，适合验证：

* 状态判断；
* 通知策略；
* 失败时对象状态；
* 关闭后排空语义。

---

## Step 2：实现阻塞式 `emplace`

在现有阻塞 `push` 基础上增加：

```cpp
queue_.emplace_back(std::forward<Args>(args)...);
```

重点测试：

* 完美转发；
* move-only 类型；
* 构造异常；
* 阻塞期间参数生命周期。

---

## Step 3：实现 `push_for`

统一使用：

```cpp
std::chrono::steady_clock
```

先实现左值版本，再实现右值版本。

重点处理：

* timeout；
* close；
* 虚假唤醒；
* 失败不移动参数。

---

## Step 4：实现 `pop_for`

复用 `pop` 的关闭语义：

```text
关闭且非空：继续消费
关闭且为空：失败
```

确保失败时输出参数保持不变。

---

## Step 5：实现 `try_emplace` 和 `emplace_for`

确认基础超时逻辑正确后，再增加原地构造的非阻塞和超时版本。

---

## Step 6：决定返回值模型

在本阶段中期决定：

```text
继续使用 bool
或
引入 QueueStatus
```

不要在所有测试和调用代码完成后再修改返回类型，否则改动范围会明显扩大。

---

## Step 7：重构内部重复逻辑

功能全部正确后，再提取：

* 等待队列非满；
* 等待队列非空；
* deadline 计算；
* 状态判断。

不要在功能尚未稳定时过早抽象。

---

## Step 8：执行完整动态检查

运行：

* Debug 单元测试；
* Release 压力测试；
* AddressSanitizer；
* UndefinedBehaviorSanitizer；
* ThreadSanitizer；
* 重复并发测试。

---

# 21. 第二阶段验收标准

## 21.1 功能验收

* [ ] `try_push` 在有空间时成功；
* [ ] `try_push` 在满队列时立即失败；
* [ ] `try_push` 在关闭后立即失败；
* [ ] `try_pop` 在有元素时成功；
* [ ] `try_pop` 在空队列时立即失败；
* [ ] `try_pop` 可以排空关闭后的剩余元素；
* [ ] `push_for` 可以在 deadline 前成功；
* [ ] `push_for` 可以正确超时；
* [ ] `push_for` 能被 `close()` 唤醒；
* [ ] `pop_for` 可以在 deadline 前成功；
* [ ] `pop_for` 可以正确超时；
* [ ] `pop_for` 能被 `close()` 唤醒；
* [ ] `emplace` 支持原地构造；
* [ ] `try_emplace` 失败时不消费参数；
* [ ] `emplace_for` 支持超时；
* [ ] 零超时行为明确；
* [ ] 负超时行为明确。

## 21.2 语义验收

* [ ] 超时使用 `steady_clock`；
* [ ] 所有等待都使用谓词；
* [ ] 多次虚假唤醒不会延长总 deadline；
* [ ] 关闭状态优先级有明确规则；
* [ ] 失败时输入对象不会被提前移动；
* [ ] 失败时输出对象保持不变；
* [ ] 关闭后仍可消费已有元素；
* [ ] 只有成功操作才发送对应通知。

## 21.3 代码质量验收

* [ ] `push` 系列没有大量重复的状态判断；
* [ ] `pop` 系列没有大量重复的状态判断；
* [ ] 模板参数完美转发正确；
* [ ] 临界区范围清晰；
* [ ] 通知位于锁外；
* [ ] 没有使用轮询代替条件变量；
* [ ] 查询接口未参与业务同步；
* [ ] 返回值语义已写入 README。

## 21.4 测试验收

* [ ] 非阻塞接口测试通过；
* [ ] 超时接口测试通过；
* [ ] 关闭与超时竞争测试通过；
* [ ] move-only 类型测试通过；
* [ ] 构造异常测试通过；
* [ ] MPMC 混合接口压力测试通过；
* [ ] ThreadSanitizer 无数据竞争；
* [ ] AddressSanitizer 无内存错误；
* [ ] 重复运行无偶现死锁。

---

# 22. 本阶段需要掌握的问题

完成第二阶段后，应能够解释以下问题：

1. 阻塞、非阻塞和超时接口分别适用于什么场景？
2. 为什么超时等待要使用 `steady_clock`？
3. `wait_for` 和 `wait_until` 有什么区别？
4. 为什么手写等待循环时更适合使用绝对 deadline？
5. 虚假唤醒会如何影响超时总时长？
6. 超时和关闭同时发生时应返回什么？
7. 为什么 `try_push` 失败时不能提前移动参数？
8. 为什么 `pop_for` 失败时不应修改输出参数？
9. `emplace` 相比 `push` 有什么价值？
10. `std::forward` 在 `emplace` 中解决了什么问题？
11. 为什么 `try_pop` 不能通过 `closed_` 判断是否失败？
12. 为什么 `empty()` 与 `try_pop()` 不是等价操作？
13. `bool` 返回值在工程中有什么局限？
14. 状态枚举如何提高接口可诊断性？
15. 如何减少多种等待接口之间的重复代码？

---

# 23. Phase 2 最终产物

第二阶段完成后，队列能力应扩展为：

```text
BlockingQueue<T>
├── 阻塞 push / pop
├── 非阻塞 try_push / try_pop
├── 超时 push_for / pop_for
├── 阻塞 emplace
├── 非阻塞 try_emplace
├── 超时 emplace_for
├── 左值复制
├── 右值移动
├── 完美转发
├── close 关闭
├── 排空后退出
├── 明确的超时语义
└── 完整并发测试
```

第二阶段的核心不是增加几个函数，而是建立一套完整的等待模型：

```text
操作条件
  │
  ├── 已满足：立即执行
  │
  ├── 不等待：立即失败
  │
  ├── 无限等待：等待条件或关闭
  │
  └── 有限等待：等待条件、关闭或 deadline
```

完成后，下一阶段可以开始替换底层 `std::deque`，实现固定容量环形存储，重点处理：

* `head`、`tail` 和 `size`；
* 固定内存；
* 对象构造与析构；
* 非默认构造类型；
* placement new；
* 缓存局部性；
* 伪共享；
* 环形下标更新；
* 异常安全。
