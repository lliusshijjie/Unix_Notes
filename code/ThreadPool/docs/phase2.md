# C++ 线程池 Phase 2：工程化增强

## 1. 阶段目标

Phase 1 已经完成了线程池的最小闭环：

- 固定数量 Worker
- 单个全局任务队列
- `submit` 返回 `std::future`
- 任务异常通过 `future` 传播
- 析构时排空已有任务
- 停止后拒绝提交

Phase 2 不追求无锁队列、工作窃取和动态扩缩容，而是把线程池从“能运行”提升到“具备基本服务端工程语义”。

本阶段重点解决四个问题：

1. 如何限制任务积压，防止内存无限增长。
2. 队列满时如何处理新任务。
3. 如何显式控制线程池关闭，而不是只依赖析构函数。
4. 如何观察线程池当前负载和运行状态。

---

## 2. 本阶段最终能力

完成后，线程池应支持：

- 有界任务队列
- 非阻塞任务提交
- 阻塞任务提交
- 明确的拒绝策略
- `post` 与 `submit`
- 显式 `shutdown`
- 排空关闭与立即关闭
- 幂等关闭
- 基础运行指标
- 更完整的并发测试

本阶段暂时不实现：

- 动态扩缩容
- 任务优先级
- 延迟任务
- 定时任务
- 工作窃取
- 无锁队列
- 强制终止正在运行的任务
- 任务依赖图
- 协程调度

---

# 3. 第一部分：将无界队列改为有界队列

## 3.1 为什么必须限制队列长度

Phase 1 中的无界队列在压力超过消费能力时，会不断积压任务。

可能产生以下问题：

- 内存持续增长
- 请求排队时间越来越长
- 已经超时的请求仍然占用资源
- 服务表面上没有拒绝请求，但实际已经不可用
- 上游继续发送请求，最终形成级联故障

因此，线程池需要增加队列容量：

```cpp
ThreadPool(std::size_t worker_count,
           std::size_t queue_capacity);
```

要求：

- `worker_count > 0`
- `queue_capacity > 0`
- 非法参数直接抛出异常
- 队列中等待任务数量不能超过容量

这里限制的是“等待执行的任务数”，不包括正在 Worker 中运行的任务。

---

## 3.2 增加第二个条件变量

Phase 1 只有 Worker 等待任务，因此通常只有一个条件变量：

```cpp
not_empty
```

Phase 2 加入阻塞提交后，生产者也需要等待队列出现空位，因此增加：

```cpp
not_full
```

两个条件变量的职责：

| 条件变量 | 等待者 | 被谁通知 |
|---|---|---|
| `not_empty` | Worker | 提交任务的线程 |
| `not_full` | 提交任务的线程 | 取走任务的 Worker |

共享状态仍然建议由同一把 mutex 保护：

- 任务队列
- 队列容量
- 线程池状态

暂时不要为了减少锁竞争而拆成多个 mutex。

---

## 3.3 Worker 取任务后的通知

Worker 从队列中取出一个任务后，队列出现了空位。

流程应为：

```text
加锁
等待队列非空或线程池停止
取出任务
解锁
通知 not_full
锁外执行任务
```

需要保持的重要原则：

> 取任务在锁内，执行任务在锁外。

任务执行完成后不需要通知 `not_full`，因为队列容量在任务被取出时就已经释放，而不是任务执行结束后才释放。

---

# 4. 第二部分：拆分任务提交接口

Phase 1 只有一个 `submit`，Phase 2 建议把提交语义拆得更明确。

---

## 4.1 `post`

`post` 用于提交不关心返回值的任务。

概念接口：

```cpp
void post(Task task);
```

适用场景：

- 异步日志
- 状态刷新
- 后台通知
- 清理任务
- 不需要结果的业务任务

需要注意：

`post` 没有 `future`，任务异常无法传回调用者。

Worker 执行 `post` 任务时，必须捕获所有异常，避免异常逃出线程入口导致 `std::terminate`。

建议提供一个异常处理器：

```cpp
using ExceptionHandler =
    std::function<void(std::exception_ptr)>;
```

默认行为可以是：

- 记录日志
- 忽略异常但保留 Worker
- 调用用户设置的异常回调

不建议让 `post` 的异常直接终止线程池。

---

## 4.2 `submit`

`submit` 继续负责有返回值任务：

```cpp
template<class F, class... Args>
auto submit(F&& f, Args&&... args)
    -> std::future<Result>;
```

它的异常语义保持不变：

- 任务执行时抛出异常
- 异常存入 `future`
- `future.get()` 时重新抛出
- Worker 继续执行后续任务

Phase 2 不需要重写 Phase 1 的任务封装，只需要把入队部分接入新的队列策略。

---

## 4.3 `try_post`

增加非阻塞提交：

```cpp
SubmitResult try_post(Task task);
```

它不等待队列空位，而是立即返回结果。

建议不要只返回 `bool`，而是定义明确结果：

```cpp
enum class SubmitResult {
    accepted,
    queue_full,
    pool_stopping
};
```

这样调用者可以区分：

- 成功提交
- 队列已满
- 线程池已停止

这对服务端错误处理很重要。

例如：

- `queue_full` 可以映射为服务过载
- `pool_stopping` 可以映射为服务正在退出

---

## 4.4 阻塞提交

可以额外提供阻塞版本：

```cpp
void post_wait(Task task);
```

基本语义：

```text
队列未满：
    立即入队

队列已满：
    等待 not_full

线程池停止：
    结束等待并返回失败或抛出异常
```

条件变量谓词必须同时检查：

```text
队列存在空位
或
线程池已经停止
```

否则线程池停止后，阻塞提交线程可能永远无法被唤醒。

---

## 4.5 超时提交

Phase 2 后半部分可以增加：

```cpp
SubmitResult post_for(Task task,
                      std::chrono::duration timeout);
```

语义：

- 队列有空位：成功提交
- 超时时间内无空位：返回 `queue_full` 或 `timeout`
- 线程池停止：返回 `pool_stopping`

如果想让结果更加准确，可以扩展为：

```cpp
enum class SubmitResult {
    accepted,
    queue_full,
    timeout,
    pool_stopping
};
```

超时提交比无限阻塞更适合服务端，因为调用线程不会永久卡住。

---

# 5. 第三部分：实现拒绝策略

## 5.1 为什么需要拒绝策略

有界队列满时，必须回答：

> 新任务应该怎么办？

如果没有明确策略，不同调用点可能自行处理，最终导致行为不一致。

建议定义：

```cpp
enum class RejectPolicy {
    abort,
    caller_runs,
    discard
};
```

Phase 2 优先实现 `abort` 和 `caller_runs`，`discard` 可以作为补充。

---

## 5.2 Abort

队列满时立即拒绝。

可选行为：

- 返回 `queue_full`
- 抛出自定义异常
- 由调用者决定是否重试

适合：

- 在线请求
- 延迟敏感业务
- 不允许无限排队
- 上游能够感知过载

建议将 Abort 作为默认策略。

原因是它的行为最明确，不会悄悄改变任务执行线程。

---

## 5.3 Caller Runs

队列满时，由提交任务的线程直接执行任务。

优点：

- 不丢任务
- 不继续增加队列压力
- 提交方会自然减速，形成背压

风险：

- 提交线程的延迟不可预测
- 任务可能在 Worker 线程，也可能在调用线程执行
- 如果调用线程是 Reactor、epoll 或 GUI 线程，可能造成严重阻塞

必须明确规定：

> Caller Runs 只能用于允许调用线程执行任务的场景。

如果线程池将用于服务端 Reactor 模型，不建议默认启用 Caller Runs。

---

## 5.4 Discard

队列满时直接丢弃任务。

只适用于：

- 非关键日志
- 指标采样
- 可覆盖的刷新任务
- 后续任务能够补偿的场景

不应静默丢弃请求任务。

如果实现 Discard，至少需要增加丢弃计数。

---

## 5.5 拒绝策略的实现边界

拒绝策略不能在持有队列锁时执行用户任务或用户回调。

错误方式：

```text
持有 mutex
调用 caller-runs 任务
```

风险：

- 任务内部再次提交导致死锁
- 任务执行时间过长，阻塞所有生产者和 Worker
- 用户回调可能调用线程池其他接口

正确思路：

```text
锁内判断队列是否已满
记录需要执行的拒绝动作
解锁
在锁外执行任务或回调
```

---

# 6. 第四部分：显式关闭接口

Phase 1 只在析构函数中关闭线程池。

Phase 2 增加显式接口：

```cpp
void shutdown(ShutdownMode mode);
void join();
```

建议状态机：

```text
running
   ↓
stopping
   ↓
stopped
```

构造函数直接进入 `running`。

Phase 2 暂不支持重新启动。

---

## 6.1 Drain 模式

定义：

```cpp
enum class ShutdownMode {
    drain,
    immediate
};
```

Drain 语义：

1. 拒绝新任务。
2. 唤醒所有等待线程。
3. Worker 继续处理队列中已有任务。
4. 队列为空后 Worker 退出。
5. `join` 等待所有 Worker 结束。

它保证：

> 已经成功入队的任务会被执行。

这是正常服务关闭时的默认模式。

---

## 6.2 Immediate 模式

Immediate 语义：

1. 拒绝新任务。
2. 清空尚未开始的任务。
3. 正在执行的任务继续完成。
4. Worker 尽快退出。
5. `join` 等待 Worker 结束。

需要明确：

> C++ 线程池无法安全强制终止正在运行的任务。

Immediate 只能取消“还在队列里的任务”，不能杀死 Worker 正在执行的任务。

---

## 6.3 被清空任务的 future

这是 Immediate 模式中的关键问题。

如果队列中存放的是带 `packaged_task` 的任务，任务对象被销毁后，对应 `future` 通常会得到 broken promise。

调用者执行 `future.get()` 时会收到 `std::future_error`。

你需要决定并记录这一语义：

- 被取消任务通过 broken promise 表示
- 或者自定义任务包装，主动设置取消异常

Phase 2 可以先接受标准库的 broken promise 行为，但必须写测试确认。

---

## 6.4 shutdown 必须幂等

以下调用都不应死锁：

```text
shutdown(drain)
shutdown(drain)
```

以及：

```text
多个线程同时调用 shutdown
```

建议规则：

- 第一次调用决定关闭模式
- 后续调用只观察当前状态
- 不重复清空任务
- 不重复执行状态转换
- `join` 不重复连接相同线程

可以简化规定：

> 如果已经进入停止状态，后续 shutdown 不改变关闭模式。

---

## 6.5 析构函数职责

析构函数仍然需要保证资源释放。

建议：

```text
如果还未关闭：
    shutdown(drain)

join 所有 Worker
```

不要在析构函数中抛出异常。

如果 `shutdown` 的内部操作可能失败，应在析构中捕获异常并采取保守处理。

---

# 7. 第五部分：状态与锁设计

## 7.1 推荐状态表示

可以使用：

```cpp
enum class State {
    running,
    stopping,
    stopped
};
```

由主 mutex 保护。

Phase 2 不建议将其直接改为 `atomic<State>`，因为：

- 状态和任务队列需要保持一致
- 入队检查和状态检查必须是原子操作
- 即使状态是 atomic，队列仍然需要加锁
- 过早使用 atomic 容易产生错误的双重同步

---

## 7.2 需要保持的不变量

实现过程中，应始终维护以下不变量。

### 不变量一

```text
state != running
→ 新任务不能成功入队
```

### 不变量二

```text
tasks.size() <= queue_capacity
```

### 不变量三

Drain 模式下：

```text
state == stopping && tasks.empty()
→ Worker 可以退出
```

### 不变量四

Immediate 模式下：

```text
state == stopping
→ Worker 不再从队列取新任务
```

### 不变量五

所有对以下对象的读写都在同一把锁下：

- `tasks`
- `state`
- `shutdown_mode`

---

# 8. 第六部分：基础可观测性

线程池用于服务端时，不能只知道“有没有运行”。

建议增加只读指标接口：

```cpp
std::size_t worker_count() const;
std::size_t queue_size() const;
std::size_t queue_capacity() const;
std::size_t active_count() const;
std::uint64_t completed_count() const;
std::uint64_t rejected_count() const;
bool is_running() const;
```

---

## 8.1 active_count

表示当前正在执行任务的 Worker 数量。

更新时机：

```text
Worker 取出任务后 active_count + 1
任务执行完成后 active_count - 1
```

任务抛异常时也必须正确减一。

适合使用 RAII 守卫保证计数恢复。

---

## 8.2 completed_count

表示已经执行完成的任务数。

需要明确：

- 成功返回算完成
- 抛出异常也算完成
- 被拒绝不算完成
- Immediate 清空的任务不算完成

---

## 8.3 rejected_count

以下情况可以计入拒绝：

- 队列已满
- 线程池已经停止
- 超时提交失败
- Discard 策略丢弃

也可以进一步拆分，但 Phase 2 先保留总计数即可。

---

## 8.4 指标读取的同步方式

可以选择：

- 使用原子计数器
- 使用 mutex 保护读取

推荐：

- 高频简单计数使用 atomic
- 队列长度仍然通过主 mutex 读取
- 不要为了得到绝对一致的指标而阻塞核心路径

指标允许是瞬时快照，不需要保证所有字段来自完全相同的时刻。

---

# 9. 推荐实现顺序

不要一次完成所有功能，按以下顺序推进。

---

## Step 1：有界队列

完成：

- `queue_capacity`
- `not_full`
- 队列长度限制
- Worker 取任务后通知生产者

暂时只保留立即拒绝，不实现阻塞提交。

验证：

- 队列不会超过容量
- 队列满时提交失败
- Worker 消费后可以继续提交

---

## Step 2：try_post

完成：

- 非阻塞提交
- `SubmitResult`
- 区分队列满和线程池停止

验证：

- 正常提交返回 accepted
- 满队列返回 queue_full
- 停止后返回 pool_stopping

---

## Step 3：阻塞与超时提交

完成：

- `post_wait`
- `post_for`
- 等待 `not_full`
- 停止时唤醒所有生产者

验证：

- 队列满时生产者阻塞
- Worker 取任务后生产者恢复
- shutdown 能唤醒阻塞生产者
- 超时后正确返回

---

## Step 4：拒绝策略

完成：

- Abort
- Caller Runs
- 可选 Discard
- rejected_count

验证：

- Caller Runs 确实在提交线程执行
- 拒绝处理在锁外完成
- Discard 不影响其他任务

---

## Step 5：显式 shutdown

完成：

- `shutdown(drain)`
- `shutdown(immediate)`
- `join`
- 状态机
- 重复调用安全

验证：

- Drain 执行完队列任务
- Immediate 清空等待任务
- 正在执行任务不会被强杀
- 停止后不能提交
- 多线程重复 shutdown 不死锁

---

## Step 6：运行指标

完成：

- queue size
- active count
- completed count
- rejected count
- running state

验证计数与测试任务数量一致。

---

# 10. 必须重点防范的问题

## 10.1 阻塞提交与关闭死锁

如果生产者正在等待 `not_full`，关闭线程池时必须：

```text
notify_all(not_full)
```

生产者被唤醒后应检查状态并退出，而不是继续等待队列空位。

---

## 10.2 Worker 内部阻塞提交

场景：

```text
所有 Worker 正在执行任务
这些任务都调用 post_wait
队列已经满
```

此时所有 Worker 都等待队列空位，但没有 Worker 能继续消费任务，形成死锁。

Phase 2 可以采用以下规定：

> 不允许 Worker 向同一线程池进行无限阻塞提交。

可选处理：

- Worker 内部提交自动使用 try_post
- 队列满时使用 Caller Runs
- 阻塞提交设置超时
- 在文档中明确禁止

推荐优先使用超时或非阻塞接口，不提供无限等待的默认 submit。

---

## 10.3 Caller Runs 不能持锁执行

Caller Runs 任务必须在锁外执行。

否则任务内部调用线程池接口时极易死锁。

---

## 10.4 shutdown 不能 join 当前线程

如果 Worker 任务内部调用 `shutdown + join`，可能发生自己等待自己。

Phase 2 可以规定：

- Worker 可以调用 `shutdown`
- Worker 不允许调用 `join`
- 线程池对象必须由外部管理线程销毁

可以通过记录 Worker 线程 ID 进行检测，并抛出逻辑错误。

---

## 10.5 清空队列时的析构开销

Immediate 模式清空大量任务时，任务析构可能很重。

不要在持有主 mutex 时进行大量复杂析构。

可以考虑：

```text
锁内将任务队列移动到临时队列
锁外销毁临时队列
```

这样能够缩短锁持有时间。

---

## 10.6 用户回调不能在锁内调用

以下行为都应在锁外：

- 异常处理器
- 拒绝回调
- Caller Runs
- 日志回调
- 任务取消回调

线程池内部锁不能跨越用户代码边界。

---

# 11. Phase 2 测试清单

## 11.1 有界队列

- 容量为 1 时行为正确
- 队列长度不会超过容量
- 队列满时 try_post 立即返回
- Worker 取任务后生产者可继续提交

---

## 11.2 阻塞提交

- 队列满时提交线程阻塞
- 队列出现空位后恢复
- 多个提交线程竞争空位
- shutdown 能唤醒所有阻塞提交线程
- 超时提交不会永久等待

---

## 11.3 拒绝策略

- Abort 正确拒绝
- Caller Runs 在调用线程执行
- Discard 不执行任务
- 拒绝计数准确
- 拒绝处理不会持有队列锁

---

## 11.4 Drain

- 关闭前已接受任务全部完成
- 关闭后新任务被拒绝
- 队列为空时能够快速退出
- 多次调用 Drain 不死锁

---

## 11.5 Immediate

- 等待队列被清空
- 正在执行任务能够自然结束
- 被清空任务的 future 得到预期异常
- 清空大量任务时不会长时间持锁

---

## 11.6 并发压力

- 多生产者并发提交
- 多线程同时 shutdown
- 提交和 shutdown 同时发生
- 极小队列容量
- 随机任务执行时间
- 随机任务抛异常
- 重复创建和销毁线程池

---

# 12. 工具与检查方式

建议使用以下编译检查：

```bash
-fsanitize=thread
-fsanitize=address
-fsanitize=undefined
```

重点关注：

- data race
- use-after-free
- 锁顺序问题
- 条件变量永久等待
- future 永久不就绪
- 任务重复执行或遗漏

性能暂时不是 Phase 2 的主要目标。

本阶段优先级：

```text
关闭语义正确
    >
队列行为正确
    >
拒绝行为明确
    >
指标准确
    >
性能优化
```

---

# 13. Phase 2 完成标准

满足以下条件即可进入 Phase 3：

1. 队列容量固定，任务不会无限堆积。
2. 支持非阻塞、阻塞和超时提交中的至少两种。
3. 队列满时行为明确。
4. 支持 Abort 和 Caller Runs。
5. 支持 Drain 与 Immediate 两种关闭模式。
6. shutdown 可重复调用且不会死锁。
7. 停止能唤醒所有 Worker 和阻塞生产者。
8. post 任务异常不会杀死 Worker。
9. Immediate 模式下，未执行任务的 future 行为明确。
10. 可以查询队列长度、活跃 Worker、完成数和拒绝数。
11. ThreadSanitizer 下没有数据竞争。
12. 压力测试中不存在任务重复、永久阻塞和无法退出。

---

# 14. 本阶段核心理解

Phase 1 的核心是：

> 如何让多个 Worker 安全消费任务。

Phase 2 的核心是：

> 当任务生产速度超过消费速度时，系统应该如何退化，并且如何安全退出。

真正具有服务端工程价值的线程池，不是能够并发执行任务，而是能够明确回答：

- 队列满了怎么办？
- 调用者会不会被阻塞？
- 哪些任务会被拒绝？
- 已接受任务是否保证执行？
- 关闭时剩余任务怎么处理？
- 哪些指标能反映线程池已经过载？

当这些语义全部清晰，并且通过并发测试验证，Phase 2 才算完成。
