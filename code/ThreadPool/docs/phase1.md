确实，上一轮信息量过大。我们现在只做**第一版线程池**，目标是先正确跑通，不加入有界队列、拒绝策略、动态扩容、工作窃取等内容。

# 第一版目标

实现一个：

> 固定线程数量、支持任务提交、支持返回值、析构时执行完剩余任务并安全退出的线程池。

第一版只解决四个问题：

1. Worker 如何等待任务。
2. 外部线程如何提交任务。
3. 任务如何返回结果和异常。
4. 线程池如何安全关闭。

---

# 第一步：确定最小接口

建议只提供三个公开接口：

```cpp
class ThreadPool {
public:
    explicit ThreadPool(std::size_t thread_count);

    ~ThreadPool();

    template<class F, class... Args>
    auto submit(F&& f, Args&&... args);

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
};
```

第一版不要提供：

* `start()`
* `shutdown()`
* `resize()`
* `try_submit()`
* 任务优先级
* 队列容量
* 取消任务

构造时创建线程，析构时关闭线程池。

---

# 第二步：确定内部成员

你需要维护这些成员：

```cpp
std::vector<std::thread> workers;
std::queue<std::function<void()>> tasks;

std::mutex mutex;
std::condition_variable condition;

bool stopping = false;
```

每个成员的职责：

* `workers`：保存工作线程，析构时需要 `join`。
* `tasks`：保存等待执行的任务。
* `mutex`：保护 `tasks` 和 `stopping`。
* `condition`：任务到来时唤醒 Worker。
* `stopping`：通知 Worker 线程池即将退出。

第一版中，`stopping` 不必使用 `atomic`，因为它和任务队列都由同一把锁保护。

---

# 第三步：先实现 Worker 循环

构造函数创建固定数量的线程，每个线程执行相同的循环：

```text
循环：
    获取互斥锁

    等待：
        队列中存在任务
        或线程池正在停止

    如果正在停止，并且任务队列为空：
        退出循环

    从队列中移动出一个任务

    释放互斥锁

    执行任务
```

这里最关键的退出条件是：

```text
stopping == true && tasks.empty()
```

不能只判断 `stopping`。

因为析构开始后，队列中可能还有已经提交的任务。第一版需要采用：

> 不再接收新任务，但把队列中的旧任务执行完成后再退出。

最重要的一条规则：

> Worker 必须在锁外执行任务。

否则所有 Worker 都会被队列锁串行化。

---

# 第四步：实现无返回值任务提交

暂时不要立刻写复杂的模板接口。

你可以先在内部验证一个简单接口：

```cpp
void post(std::function<void()> task);
```

提交过程：

```text
获取锁
检查线程池是否正在停止
任务放入队列
释放锁
condition.notify_one()
```

先验证下面的功能：

* 创建 4 个 Worker。
* 连续提交 100 个 Lambda。
* 所有任务都能执行。
* 不出现重复执行或遗漏。

这个阶段跑通后，再实现真正的 `submit`。

---

# 第五步：实现带返回值的 submit

最终的 `submit` 应支持：

```cpp
auto result = pool.submit([](int a, int b) {
    return a + b;
}, 1, 2);

result.get();
```

实现思路：

```text
F 和 Args...
    ↓
绑定为一个无参数可调用对象
    ↓
包装到 packaged_task<R()>
    ↓
获取 future<R>
    ↓
再包装成 void() 任务放入队列
    ↓
返回 future<R>
```

需要使用的现代 C++ 工具：

* `std::invoke_result_t`
* `std::forward`
* `std::bind`，或者 Lambda 捕获参数
* `std::packaged_task`
* `std::future`
* `std::make_shared`

为什么需要 `shared_ptr<packaged_task>`：

`std::packaged_task` 只能移动，而第一版队列使用的 `std::function<void()>` 通常要求任务可复制。因此可以让 Lambda 捕获一个 `shared_ptr`。

第一版接受这次堆分配，不做性能优化。

---

# 第六步：实现析构退出

析构函数按以下顺序执行：

```text
获取锁
设置 stopping = true
释放锁

condition.notify_all()

依次 join 所有 Worker
```

必须先修改停止状态，再通知 Worker。

否则 Worker 被唤醒后看不到退出条件，可能重新进入等待，导致析构永久阻塞。

---

# 第七步：规定 submit 的停止语义

当 `stopping == true` 后调用 `submit`，第一版直接：

```text
抛出 std::runtime_error
```

不要静默忽略任务，也不要允许任务继续进入队列。

提交任务时，对状态检查和任务入队必须放在同一次加锁区间中：

```text
加锁
检查 stopping
入队
解锁
```

否则可能出现：

```text
提交线程检查到未停止
析构线程设置停止
提交线程又把任务放入队列
Worker 已经退出
任务永远无法执行
```

---

# 第八步：第一版只做这些测试

按顺序测试，不要一次性全部编写。

## 测试一：基本执行

提交 1000 个任务，用原子计数器计数，最终必须等于 1000。

## 测试二：返回值

提交加法、字符串拼接、`void` 函数，验证 `future.get()`。

## 测试三：异常传播

任务内部抛出异常：

```text
submit 不应该抛出任务异常
future.get() 应该抛出任务异常
Worker 不能因此退出
```

## 测试四：并发提交

启动多个生产者线程，同时向线程池提交任务，检查所有任务恰好执行一次。

## 测试五：析构排空

提交一批包含短暂休眠的任务，立即销毁线程池，确认已提交任务全部执行完成。

## 测试六：零线程参数

构造时传入 `0` 应直接抛异常，避免任务永久留在队列中。

---

# 第一版完成标准

满足以下条件就停止，不继续加功能：

* 固定数量 Worker。
* 单个全局任务队列。
* 支持任意可调用对象和参数。
* 支持返回值和 `void`。
* 支持通过 `future` 传播异常。
* 多线程可以并发提交。
* 析构时排空已有任务。
* 停止后拒绝提交。
* 没有数据竞争和死锁。

第一版的实现顺序就是：

```text
Worker 循环
    ↓
简单 post
    ↓
submit + future
    ↓
析构退出
    ↓
并发与异常测试
```

先不要考虑任何优化。第一版真正需要吃透的是：**条件变量谓词、锁的保护范围、任务移动、关闭状态与队列的一致性。**
