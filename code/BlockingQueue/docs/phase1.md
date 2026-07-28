# 有界阻塞队列 Phase 1：基础正确性版本

## 1. 阶段目标

本阶段实现一个通用的、有界的、线程安全的阻塞队列：

```cpp
template <typename T>
class BlockingQueue;
```

队列支持多个生产者线程和多个消费者线程并发访问，即：

```text
MPMC：Multiple Producer Multiple Consumer
```

底层暂时使用：

```cpp
std::deque<T>
std::mutex
std::condition_variable
```

第一阶段不追求极致性能，核心目标是建立完整、明确、可验证的并发语义。

需要重点解决以下问题：

1. 队列为空时，消费者如何阻塞等待。
2. 队列已满时，生产者如何阻塞等待。
3. 多生产者、多消费者并发访问是否安全。
4. 队列关闭后，阻塞线程如何退出。
5. 队列中已有元素是否允许继续消费。
6. 如何避免虚假唤醒、丢失唤醒和永久阻塞。
7. 如何支持只能移动、不能复制的类型。
8. 队列析构前如何保证不存在仍在访问它的线程。

---

# 2. 本阶段范围

## 2.1 必须完成

第一阶段实现以下能力：

* 构造固定容量的队列。
* 阻塞式 `push`。
* 阻塞式 `pop`。
* 支持左值复制入队。
* 支持右值移动入队。
* 支持多生产者。
* 支持多消费者。
* 支持主动关闭队列。
* `close()` 多次调用保持幂等。
* 队列关闭后唤醒所有等待线程。
* 队列关闭后禁止继续入队。
* 队列关闭后允许消费剩余元素。
* 队列关闭且为空时，`pop` 返回失败。
* 提供基础状态查询接口。
* 完成单线程和多线程测试。
* 使用 ThreadSanitizer 或类似工具检查数据竞争。

## 2.2 暂不实现

以下内容留到后续阶段：

* `try_push`、`try_pop`。
* 带超时的 `push_for`、`pop_for`。
* 批量入队、批量出队。
* 环形数组存储。
* 原始内存和 placement new。
* 无锁队列。
* 自旋锁。
* 信号量实现。
* 动态容量调整。
* 优先级队列。
* 公平调度保证。
* 精细化性能指标统计。

第一阶段应避免功能膨胀。先证明队列的并发行为正确，再逐步增加功能。

---

# 3. 使用场景

该队列是服务端并发基础设施，可以用于：

```text
生产者线程
    │
    │ push
    ▼
BlockingQueue<T>
    │
    │ pop
    ▼
消费者线程
```

典型场景包括：

* 线程池任务队列。
* 异步日志缓冲区。
* 网络线程向业务线程投递请求。
* 业务线程向存储线程投递写任务。
* 数据处理流水线。
* 后台批量任务系统。
* 生产者—消费者模型练习。

后续可以将线程池中的私有任务队列替换为：

```cpp
BlockingQueue<MoveOnlyFunction<void()>> task_queue_;
```

---

# 4. 推荐目录结构

```text
blocking_queue/
├── include/
│   └── blocking_queue.hpp
├── tests/
│   └── blocking_queue_test.cpp
├── examples/
│   └── producer_consumer.cpp
├── CMakeLists.txt
└── README.md
```

由于 `BlockingQueue` 是类模板，第一阶段建议直接将实现放在头文件中：

```text
blocking_queue.hpp
```

不建议将模板声明和实现分别放入普通 `.hpp` 和 `.cpp` 文件，否则容易出现模板实例化和链接问题。

---

# 5. 类接口设计

第一阶段推荐接口如下：

```cpp
template <typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(std::size_t capacity);

    ~BlockingQueue() = default;

    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    BlockingQueue(BlockingQueue&&) = delete;
    BlockingQueue& operator=(BlockingQueue&&) = delete;

    bool push(const T& value);
    bool push(T&& value);

    bool pop(T& value);

    void close();

    bool closed() const;
    bool empty() const;
    bool full() const;

    std::size_t size() const;
    std::size_t capacity() const;

private:
    std::size_t capacity_;
    std::deque<T> queue_;

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;

    bool closed_{false};
};
```

---

# 6. 接口语义

## 6.1 构造函数

```cpp
explicit BlockingQueue(std::size_t capacity);
```

构造一个容量固定的阻塞队列。

约束：

```text
capacity > 0
```

对于 `capacity == 0`，推荐直接抛出：

```cpp
std::invalid_argument
```

原因是零容量队列的语义并不属于当前项目范围。

成员初始化建议：

```cpp
capacity_(capacity)
closed_(false)
```

队列初始状态：

```text
未关闭
元素数量为 0
允许生产者写入
消费者在 pop 时需要等待
```

---

## 6.2 `push(const T&)`

```cpp
bool push(const T& value);
```

将一个左值复制到队列中。

行为：

1. 获取互斥锁。
2. 队列已满且未关闭时，当前线程阻塞。
3. 被唤醒后重新检查条件。
4. 如果队列已经关闭，返回 `false`。
5. 否则将元素复制到队列尾部。
6. 释放互斥锁。
7. 唤醒一个等待元素的消费者。
8. 返回 `true`。

返回值：

```text
true：入队成功
false：队列已经关闭，入队失败
```

该接口要求 `T` 可以复制构造。

---

## 6.3 `push(T&&)`

```cpp
bool push(T&& value);
```

将右值移动到队列中。

语义与左值版本一致，但入队时使用：

```cpp
queue_.push_back(std::move(value));
```

该接口允许队列存储只能移动的对象，例如：

```cpp
std::unique_ptr<T>
std::packaged_task<void()>
MoveOnlyFunction<void()>
```

这是线程池任务队列的重要基础。

---

## 6.4 `pop(T&)`

```cpp
bool pop(T& value);
```

从队列头部取出一个元素。

行为：

1. 获取互斥锁。
2. 队列为空且未关闭时，当前线程阻塞。
3. 被唤醒后重新检查条件。
4. 如果队列为空且已经关闭，返回 `false`。
5. 否则将队头元素移动给输出参数。
6. 删除队头元素。
7. 释放互斥锁。
8. 唤醒一个等待空间的生产者。
9. 返回 `true`。

返回值：

```text
true：成功取出一个元素
false：队列已经关闭，并且没有剩余元素
```

消费者推荐使用以下形式：

```cpp
T value;

while (queue.pop(value)) {
    process(value);
}
```

当队列被关闭且剩余元素全部消费完成后，循环自然结束。

---

## 6.5 `close()`

```cpp
void close();
```

关闭队列。

关闭语义是第一阶段最重要的设计之一。

调用 `close()` 后：

* 不再允许新的元素入队。
* 队列中已有元素不会被清空。
* 消费者仍然可以取出剩余元素。
* 阻塞在 `push` 中的生产者全部被唤醒。
* 阻塞在 `pop` 中的消费者全部被唤醒。
* 队列为空后，后续 `pop` 返回 `false`。
* 多次调用 `close()` 不产生额外副作用。

推荐逻辑：

```text
加锁
  ↓
closed_ 已经为 true？
  ├─ 是：直接结束
  └─ 否：设置 closed_ = true
  ↓
解锁
  ↓
not_empty_.notify_all()
not_full_.notify_all()
```

通知操作可以放在解锁之后执行，减少被唤醒线程立即竞争同一把锁的概率。

---

# 7. 关闭语义状态表

假设当前线程调用 `push`：

| 队列状态 | 是否有空间 | 行为         |
| ---- | ----: | ---------- |
| 未关闭  |   有空间 | 立即入队       |
| 未关闭  |   无空间 | 阻塞等待       |
| 已关闭  |   有空间 | 返回 `false` |
| 已关闭  |   无空间 | 返回 `false` |

假设当前线程调用 `pop`：

| 队列状态 | 是否为空 | 行为         |
| ---- | ---: | ---------- |
| 未关闭  |    否 | 立即取出元素     |
| 未关闭  |    是 | 阻塞等待       |
| 已关闭  |    否 | 继续取出剩余元素   |
| 已关闭  |    是 | 返回 `false` |

最关键的一点是：

```text
closed_ == true
```

并不意味着 `pop` 一定失败。

只有下面两个条件同时成立时，`pop` 才失败：

```text
closed_ == true
queue_.empty() == true
```

---

# 8. 核心状态不变量

实现过程中应始终保证以下不变量。

## 8.1 容量不变量

```cpp
queue_.size() <= capacity_
```

任何时候队列元素数量都不能超过容量。

## 8.2 容量有效性

```cpp
capacity_ > 0
```

构造完成后容量固定，不允许变化。

## 8.3 关闭状态不可逆

```text
false → true
```

`closed_` 只能从未关闭变为关闭，不能重新打开。

不支持：

```text
true → false
```

如果需要重新使用，应创建新的队列对象。

## 8.4 共享状态受锁保护

以下成员只能在持有 `mutex_` 时访问：

```cpp
queue_
closed_
```

`capacity_` 在构造后不再修改，因此读取时不需要锁。

## 8.5 条件变量不保存状态

条件变量只负责通知，不负责保存“队列是否为空”这样的状态。

真正的状态保存在：

```cpp
queue_
closed_
```

因此每次被唤醒后都必须重新判断条件。

---

# 9. 条件变量设计

队列需要两个条件变量：

```cpp
std::condition_variable not_empty_;
std::condition_variable not_full_;
```

## 9.1 `not_empty_`

消费者等待队列中出现元素，或者队列被关闭。

等待谓词：

```cpp
closed_ || !queue_.empty()
```

逻辑含义：

```text
有元素可以消费
或者
队列关闭，消费者需要退出等待并重新判断
```

不能只等待：

```cpp
!queue_.empty()
```

否则当队列为空时调用 `close()`，消费者无法满足谓词，也就不能正常退出。

## 9.2 `not_full_`

生产者等待队列出现空闲容量，或者队列被关闭。

等待谓词：

```cpp
closed_ || queue_.size() < capacity_
```

不能只等待：

```cpp
queue_.size() < capacity_
```

否则队列已满时调用 `close()`，生产者即使被唤醒，也可能继续等待空间，最终永久阻塞。

---

# 10. 为什么必须使用谓词等待

错误形式：

```cpp
if (queue_.empty()) {
    not_empty_.wait(lock);
}
```

该写法存在两个问题。

## 10.1 虚假唤醒

条件变量允许线程在没有对应通知的情况下被唤醒。

线程被唤醒并不代表：

```cpp
!queue_.empty()
```

已经成立。

## 10.2 条件可能再次失效

多个消费者同时被唤醒后，某个消费者可能先抢到锁并取走元素。

后续消费者获得锁时，队列可能再次为空。

因此应采用谓词版本：

```cpp
not_empty_.wait(lock, [&] {
    return closed_ || !queue_.empty();
});
```

它等价于：

```cpp
while (!closed_ && queue_.empty()) {
    not_empty_.wait(lock);
}
```

并发代码中必须以状态条件为依据，不能以“收到通知”为依据。

---

# 11. `push` 核心流程

## 11.1 左值版本

逻辑流程：

```text
调用 push
    ↓
获取 mutex_
    ↓
等待：队列未满，或者队列已关闭
    ↓
检查 closed_
    ├─ true：返回 false
    └─ false：继续
    ↓
queue_.push_back(value)
    ↓
释放 mutex_
    ↓
not_empty_.notify_one()
    ↓
返回 true
```

伪代码：

```cpp
bool push(const T& value)
{
    std::unique_lock<std::mutex> lock(mutex_);

    not_full_.wait(lock, [this] {
        return closed_ || queue_.size() < capacity_;
    });

    if (closed_) {
        return false;
    }

    queue_.push_back(value);

    lock.unlock();
    not_empty_.notify_one();

    return true;
}
```

## 11.2 右值版本

右值版本只在真正入队时移动：

```cpp
queue_.push_back(std::move(value));
```

在等待期间，不能提前移动参数。

也就是说，不应先执行：

```cpp
T temp = std::move(value);
```

然后再阻塞等待，因为队列可能在等待期间被关闭，最终入队失败，但调用者传入的对象已经被移动。

第一阶段推荐仅在确定可以入队后执行移动。

---

# 12. `pop` 核心流程

逻辑流程：

```text
调用 pop
    ↓
获取 mutex_
    ↓
等待：队列非空，或者队列已关闭
    ↓
检查 queue_.empty()
    ├─ true：说明已关闭且无剩余元素，返回 false
    └─ false：继续
    ↓
移动队头元素到 value
    ↓
queue_.pop_front()
    ↓
释放 mutex_
    ↓
not_full_.notify_one()
    ↓
返回 true
```

伪代码：

```cpp
bool pop(T& value)
{
    std::unique_lock<std::mutex> lock(mutex_);

    not_empty_.wait(lock, [this] {
        return closed_ || !queue_.empty();
    });

    if (queue_.empty()) {
        return false;
    }

    value = std::move(queue_.front());
    queue_.pop_front();

    lock.unlock();
    not_full_.notify_one();

    return true;
}
```

这里判断的是：

```cpp
queue_.empty()
```

而不是简单判断：

```cpp
closed_
```

因为关闭后仍然需要消费已有元素。

---

# 13. 为什么使用 `std::unique_lock`

条件变量的 `wait` 需要：

```cpp
std::unique_lock<std::mutex>
```

原因是等待过程内部需要完成以下原子化语义：

```text
释放 mutex
进入等待状态
收到通知
重新获取 mutex
返回调用方
```

`std::lock_guard` 不支持中途解锁和重新加锁，因此不能直接用于 `condition_variable::wait`。

在普通查询接口中，则可以使用更轻量的：

```cpp
std::lock_guard<std::mutex>
```

或者：

```cpp
std::scoped_lock
```

---

# 14. 通知策略

## 14.1 入队成功后

每次成功入队后：

```cpp
not_empty_.notify_one();
```

因为新增了一个元素，通常只需要唤醒一个消费者。

## 14.2 出队成功后

每次成功出队后：

```cpp
not_full_.notify_one();
```

因为新增了一个空位，通常只需要唤醒一个生产者。

## 14.3 关闭队列后

```cpp
not_empty_.notify_all();
not_full_.notify_all();
```

关闭是全局状态变化。

所有等待中的生产者和消费者都需要重新检查状态，因此必须使用 `notify_all`。

如果关闭时只调用 `notify_one`，其他线程可能永久处于等待状态。

---

# 15. 先解锁还是先通知

推荐采用：

```cpp
lock.unlock();
condition.notify_one();
```

而不是：

```cpp
condition.notify_one();
lock.unlock();
```

原因是被唤醒的线程通常需要获取同一把互斥锁。

如果当前线程仍持有锁，被唤醒线程会立即进入锁竞争，然后再次阻塞。

先解锁再通知可以减少无意义竞争。

但要注意：这不是正确性的必要条件。只要状态修改在锁内完成，两种顺序通常都不会造成丢失唤醒。

本项目统一采用：

```text
锁内修改共享状态
锁外发送通知
```

---

# 16. 查询接口

## 16.1 `closed()`

```cpp
bool closed() const;
```

读取 `closed_` 时必须加锁。

```cpp
std::lock_guard<std::mutex> lock(mutex_);
return closed_;
```

## 16.2 `empty()`

```cpp
bool empty() const;
```

返回调用瞬间队列是否为空。

该状态只是瞬时快照。函数返回后，其他线程可能立即执行 `push` 或 `pop`。

不能编写：

```cpp
if (!queue.empty()) {
    queue.pop(value);
}
```

并期望 `pop` 一定不会阻塞。

正确性必须由 `pop` 本身保证。

## 16.3 `full()`

```cpp
bool full() const;
```

判断：

```cpp
queue_.size() == capacity_
```

同样只是瞬时快照。

## 16.4 `size()`

```cpp
std::size_t size() const;
```

需要加锁，返回当前元素数量。

## 16.5 `capacity()`

```cpp
std::size_t capacity() const noexcept;
```

容量构造后不再改变，因此可直接返回：

```cpp
return capacity_;
```

---

# 17. 类型要求

第一阶段不强制使用 C++20 Concept，但需要理解不同接口对 `T` 的约束。

## 17.1 左值 `push`

```cpp
bool push(const T& value);
```

要求：

```text
T 可以复制构造
```

## 17.2 右值 `push`

```cpp
bool push(T&& value);
```

要求：

```text
T 可以移动构造
```

## 17.3 `pop(T&)`

如果采用：

```cpp
value = std::move(queue_.front());
```

则要求：

```text
T 可以移动赋值
```

这意味着某些“可移动构造但不可移动赋值”的类型无法使用当前接口。

这是第一阶段可以接受的限制，但应在 README 中明确说明。

后续阶段可以考虑返回：

```cpp
std::optional<T>
```

或者通过回调消费元素，从而降低对移动赋值的要求。

---

# 18. 异常安全

## 18.1 `push` 异常

以下操作可能抛出异常：

```cpp
queue_.push_back(value);
queue_.push_back(std::move(value));
```

例如：

* 内存分配失败。
* `T` 的复制构造抛出异常。
* `T` 的移动构造抛出异常。

如果入队操作抛出异常：

* `unique_lock` 会自动释放互斥锁。
* 队列应维持原有状态。
* 不应发送 `not_empty_` 通知。
* 异常直接向上传递。

`std::deque::push_back` 通常可以提供较强的异常保证，但具体仍取决于 `T`。

## 18.2 `pop` 异常

以下操作可能抛出：

```cpp
value = std::move(queue_.front());
```

如果移动赋值抛出异常，应确保：

```cpp
queue_.pop_front();
```

尚未执行。

因此顺序必须是：

```cpp
value = std::move(queue_.front());
queue_.pop_front();
```

不能先删除队头，再向输出参数赋值，否则异常发生时元素会丢失。

需要认识到，即使没有执行 `pop_front()`，抛异常的移动赋值也可能已经部分修改队头对象。对于移动操作可能抛异常的复杂类型，第一阶段不承诺强异常安全。

在线程池、日志队列等常见场景中，通常优先存储具备 `noexcept` 移动能力的对象。

---

# 19. 生命周期约束

`BlockingQueue` 本身不能保证“析构时自动安全停止所有线程”。

调用方必须确保：

1. 调用 `close()`。
2. 等待所有生产者和消费者线程退出。
3. 最后析构队列。

正确顺序：

```text
close queue
    ↓
join producer threads
    ↓
join consumer threads
    ↓
destroy queue
```

错误顺序：

```text
destroy queue
    ↓
worker thread 仍在执行 push/pop
```

后一种情况会访问已经销毁的互斥锁、条件变量和容器，属于未定义行为。

第一阶段可以保持默认析构函数，但必须在文档中明确生命周期责任。

---

# 20. 为什么禁用复制和移动

队列内部包含：

```cpp
std::mutex
std::condition_variable
```

它们本身不可复制。

同时，一个正在被多个线程等待和访问的并发对象，也不适合在运行期间改变内存地址。

因此应显式禁用：

```cpp
BlockingQueue(const BlockingQueue&) = delete;
BlockingQueue& operator=(const BlockingQueue&) = delete;

BlockingQueue(BlockingQueue&&) = delete;
BlockingQueue& operator=(BlockingQueue&&) = delete;
```

这样可以防止调用方错误地尝试移动一个正在使用的队列。

---

# 21. 线程交互示例

假设容量为 2。

初始状态：

```text
queue = []
closed = false
```

生产者依次写入：

```text
push(A) → [A]
push(B) → [A, B]
```

第三次调用：

```text
push(C)
```

由于队列已满，生产者阻塞。

消费者执行：

```text
pop() → A
```

状态变为：

```text
queue = [B]
```

消费者执行完 `pop` 后调用：

```cpp
not_full_.notify_one();
```

生产者被唤醒，写入 C：

```text
queue = [B, C]
```

随后主线程调用：

```cpp
close();
```

状态变为：

```text
queue = [B, C]
closed = true
```

消费者仍然可以依次取出：

```text
B
C
```

当队列变为空时：

```text
queue = []
closed = true
```

下一次 `pop` 返回 `false`。

---

# 22. 第一阶段测试计划

测试不能只验证单线程功能，必须覆盖关闭、阻塞和并发行为。

## 22.1 构造测试

验证：

* 正常容量可以构造。
* 容量为 1 可以构造。
* 容量为 0 抛出异常。
* 初始状态为空。
* 初始状态未关闭。
* 初始大小为 0。
* `capacity()` 返回构造值。

---

## 22.2 单线程 FIFO 测试

依次入队：

```text
1, 2, 3
```

依次出队，应得到：

```text
1, 2, 3
```

验证队列保持先进先出语义。

---

## 22.3 左值复制测试

定义一个可复制对象：

```cpp
std::string value = "hello";
```

执行：

```cpp
queue.push(value);
```

验证：

* 入队成功。
* 原对象内容保持不变。
* 出队内容为 `"hello"`。

---

## 22.4 右值移动测试

使用：

```cpp
std::unique_ptr<int>
```

验证队列支持 move-only 类型。

流程：

```text
创建 unique_ptr
    ↓
移动入队
    ↓
原 unique_ptr 为空
    ↓
成功出队
    ↓
值保持正确
```

---

## 22.5 满队列阻塞测试

容量设置为 1。

步骤：

1. 主线程先写入一个元素。
2. 启动生产者线程继续调用 `push`。
3. 验证生产者尚未完成。
4. 主线程执行一次 `pop`。
5. 验证生产者随后成功完成。

该测试用于证明：

```text
队列满时生产者阻塞
队列腾出空间后生产者被唤醒
```

不要仅依靠长时间 `sleep` 判断。

可以使用：

```cpp
std::promise
std::future
std::latch
std::atomic<bool>
```

配合短暂等待控制测试流程。

---

## 22.6 空队列阻塞测试

步骤：

1. 创建空队列。
2. 启动消费者线程执行 `pop`。
3. 验证消费者尚未完成。
4. 主线程写入一个元素。
5. 验证消费者被唤醒并取得正确元素。

---

## 22.7 关闭空队列测试

步骤：

1. 创建空队列。
2. 启动消费者线程执行 `pop`。
3. 消费者进入等待。
4. 主线程调用 `close()`。
5. 消费者应返回 `false` 并退出。

该测试非常重要，可以发现 `pop` 的等待谓词是否遗漏了：

```cpp
closed_
```

---

## 22.8 关闭满队列测试

步骤：

1. 创建容量为 1 的队列。
2. 写入一个元素，使队列满。
3. 启动另一个生产者继续执行 `push`。
4. 生产者进入等待。
5. 主线程调用 `close()`。
6. 阻塞中的生产者应返回 `false`。

该测试可以发现 `push` 的等待谓词是否遗漏了：

```cpp
closed_
```

---

## 22.9 关闭后继续消费测试

步骤：

1. 写入多个元素。
2. 调用 `close()`。
3. 继续调用 `pop`。
4. 验证所有已有元素均可被取出。
5. 元素耗尽后，下一次 `pop` 返回 `false`。

---

## 22.10 关闭后禁止入队测试

步骤：

1. 调用 `close()`。
2. 分别调用左值和右值 `push`。
3. 两者都应返回 `false`。
4. 队列大小保持为 0。

对于右值版本，还可以验证入队失败时参数没有被提前移动。

---

## 22.11 重复关闭测试

连续调用：

```cpp
queue.close();
queue.close();
queue.close();
```

验证：

* 不崩溃。
* 不抛异常。
* 状态保持关闭。
* `pop`、`push` 行为符合关闭语义。

---

## 22.12 多生产者多消费者测试

建议配置：

```text
4 个生产者
4 个消费者
每个生产者写入 10,000 个整数
```

每个元素可以编码为：

```text
producer_id × N + sequence
```

所有消费者将取出的值记录到线程安全结果容器中，最终验证：

* 成功消费总数正确。
* 没有元素丢失。
* 没有元素重复。
* 所有生产者写入完成。
* 所有消费者在关闭后正常退出。

整体流程：

```text
启动消费者
    ↓
启动生产者
    ↓
等待所有生产者完成
    ↓
调用 queue.close()
    ↓
等待所有消费者完成
    ↓
校验结果
```

注意不能在生产者完成之前关闭队列。

---

# 23. 测试同步建议

并发测试中应尽量避免依靠固定睡眠：

```cpp
std::this_thread::sleep_for(...)
```

固定睡眠容易产生：

* 测试偶现失败。
* 不同机器执行速度不同。
* 测试时间过长。
* 测试并未真正进入预期状态。

更推荐使用：

```cpp
std::promise
std::future
std::latch
std::barrier
std::atomic
```

例如：

```text
消费者启动
    ↓
消费者通过 promise 通知“即将执行 pop”
    ↓
主线程确认消费者已启动
    ↓
主线程执行后续测试动作
```

需要注意：即使线程通知“即将执行 pop”，也不能绝对证明它已经阻塞在条件变量中。因此并发测试应重点验证最终可观测行为，而不是依赖内部执行时序。

---

# 24. 工具检查

## 24.1 ThreadSanitizer

在 Linux + GCC/Clang 环境中建议开启：

```bash
-fsanitize=thread
-g
-O1
```

运行多生产者、多消费者压力测试，检查：

* 数据竞争。
* 未正确加锁的共享访问。
* 锁使用问题。

不要同时启用 AddressSanitizer 和 ThreadSanitizer，它们通常不能组合使用。

## 24.2 AddressSanitizer

单独构建 ASan 版本：

```bash
-fsanitize=address,undefined
-g
```

检查：

* 越界访问。
* Use-after-free。
* 未定义行为。
* 生命周期错误。

## 24.3 重复压力测试

可以循环执行测试：

```bash
for i in {1..100}; do
    ./blocking_queue_test || exit 1
done
```

并发错误往往不是每次都能复现。

---

# 25. 常见错误

## 25.1 使用 `if` 代替谓词或 `while`

错误：

```cpp
if (queue_.empty()) {
    not_empty_.wait(lock);
}
```

问题：

* 无法处理虚假唤醒。
* 无法处理多个消费者竞争。
* 被唤醒时条件可能已经失效。

---

## 25.2 `close()` 只唤醒消费者

错误：

```cpp
not_empty_.notify_all();
```

遗漏：

```cpp
not_full_.notify_all();
```

后果是阻塞在满队列上的生产者无法退出。

---

## 25.3 `pop` 看到关闭就立即失败

错误：

```cpp
if (closed_) {
    return false;
}
```

后果是关闭前已写入的元素被直接丢弃。

正确失败条件是：

```cpp
closed_ && queue_.empty()
```

在完成等待后，可以直接判断：

```cpp
queue_.empty()
```

因为此时谓词已经保证：

```text
closed_ 或 queue 非空
```

---

## 25.4 在锁外读取 `queue_`

错误：

```cpp
if (queue_.empty()) {
    ...
}
```

如果没有持有 `mutex_`，其他线程可能同时修改 `queue_`，导致数据竞争。

---

## 25.5 在等待前提前移动参数

错误：

```cpp
T temp = std::move(value);

not_full_.wait(...);

if (closed_) {
    return false;
}
```

即使最终入队失败，调用方对象也已经被移动。

---

## 25.6 关闭时清空队列

第一阶段关闭语义要求“排空后退出”。

因此 `close()` 不应执行：

```cpp
queue_.clear();
```

清空队列属于另一种停止策略，应设计为不同接口，例如：

```cpp
cancel()
abort()
close_and_discard()
```

本阶段不实现这些接口。

---

## 25.7 用原子变量替代所有加锁

将 `closed_` 改为：

```cpp
std::atomic<bool>
```

并不能让整体实现不再需要互斥锁。

队列容器、容量条件以及关闭状态需要作为一个整体进行判断：

```text
队列是否满
队列是否空
队列是否关闭
```

第一阶段统一使用同一把互斥锁保护全部共享状态，更容易保证状态一致性。

---

# 26. 开发顺序

## Step 1：建立项目结构

完成：

* `blocking_queue.hpp`
* 测试文件
* CMake 配置
* 基础编译选项

建议开启：

```text
-Wall
-Wextra
-Wpedantic
-Werror
```

根据实际编译器适当调整。

## Step 2：实现构造与查询接口

先完成：

* 构造函数。
* `capacity()`。
* `size()`。
* `empty()`。
* `full()`。
* `closed()`。

编写对应单线程测试。

## Step 3：实现基础 `push`

先实现：

```cpp
bool push(const T&);
bool push(T&&);
```

暂时配合单线程测试验证：

* FIFO。
* 左值复制。
* 右值移动。
* 容量不超过限制。

由于阻塞 `push` 单独测试容易挂死，应尽快进入消费者实现。

## Step 4：实现 `pop`

实现完整阻塞逻辑，并完成：

* 空队列阻塞。
* 满队列唤醒。
* FIFO。
* move-only 类型测试。

## Step 5：实现 `close`

完成：

* 关闭空队列。
* 关闭满队列。
* 唤醒全部线程。
* 排空后退出。
* 重复关闭。

## Step 6：完成 MPMC 压力测试

启动多个生产者和消费者，校验：

* 总数量。
* 唯一性。
* 完整性。
* 正常退出。

## Step 7：运行动态检查工具

分别运行：

* 普通 Debug 测试。
* ASan + UBSan 测试。
* TSan 测试。
* Release 压力测试。

---

# 27. 第一阶段验收标准

完成以下条件，才能认为 Phase 1 结束。

## 功能验收

* [ ] 容量在构造时固定。
* [ ] 容量为 0 时拒绝构造。
* [ ] 队列遵循 FIFO。
* [ ] 队列满时生产者阻塞。
* [ ] 队列空时消费者阻塞。
* [ ] `push` 支持左值。
* [ ] `push` 支持右值。
* [ ] 支持 move-only 类型。
* [ ] 多生产者并发安全。
* [ ] 多消费者并发安全。
* [ ] 关闭后禁止写入。
* [ ] 关闭后已有元素仍可消费。
* [ ] 关闭且为空时 `pop` 返回 `false`。
* [ ] `close()` 可以重复调用。
* [ ] 关闭会唤醒所有等待线程。

## 代码质量验收

* [ ] 所有共享状态均受互斥锁保护。
* [ ] 条件变量统一使用谓词等待。
* [ ] 锁的持有范围清晰。
* [ ] 通知操作不在无意义的长临界区内执行。
* [ ] 队列类禁止复制和移动。
* [ ] 接口返回值语义清晰。
* [ ] 生命周期约束写入 README。
* [ ] 没有使用魔法睡眠维持业务正确性。

## 测试验收

* [ ] 单线程测试通过。
* [ ] 阻塞行为测试通过。
* [ ] 关闭语义测试通过。
* [ ] move-only 类型测试通过。
* [ ] MPMC 压力测试通过。
* [ ] ThreadSanitizer 无数据竞争报告。
* [ ] AddressSanitizer 无内存错误报告。
* [ ] 重复运行测试无偶现死锁。

---

# 28. 本阶段需要真正掌握的知识

完成这一阶段后，不应只停留在“代码能够运行”，还应能够解释以下问题：

1. 为什么条件变量必须和互斥锁配合使用？
2. 条件变量的通知是否会保存？
3. 什么是虚假唤醒？
4. 为什么必须使用谓词或 `while` 重新检查状态？
5. 为什么队列需要 `not_empty` 和 `not_full` 两个条件变量？
6. 为什么关闭时需要 `notify_all`？
7. 为什么 `pop` 不能看到 `closed_` 就立即失败？
8. 为什么共享状态必须在锁内修改？
9. 为什么通知可以放到解锁之后？
10. 为什么查询接口只能返回瞬时状态？
11. 为什么析构函数不能自动解决外部线程生命周期？
12. 为什么第一阶段不应该直接实现无锁队列？
13. 如何验证多生产者、多消费者下不存在元素丢失和重复？
14. move-only 类型对接口设计有什么影响？
15. `pop(T&)` 对 `T` 的移动赋值能力提出了什么要求？

---

# 29. Phase 1 最终产物

第一阶段完成后，项目应得到一个具备以下特征的组件：

```text
BlockingQueue<T>
├── 固定容量
├── 多生产者
├── 多消费者
├── 阻塞 push
├── 阻塞 pop
├── 左值复制
├── 右值移动
├── FIFO
├── close 关闭
├── 排空后退出
└── 完整并发测试
```

该版本的价值不在于性能极致，而在于并发语义完整、边界行为明确、关闭流程可靠。

完成后可以进入 Phase 2，增加：

```text
try_push / try_pop
push_for / pop_for
emplace
更完整的错误表达
批量操作
状态统计
```

再进入 Phase 3，将 `std::deque` 替换为固定容量环形存储，重点处理对象生命周期、原始内存和缓存局部性。
