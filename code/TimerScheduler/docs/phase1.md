# 定时器调度器：第一阶段实现框架

## 阶段目标

实现一个**基础的单线程定时器调度器**，支持一次性延迟任务：

```text
提交任务 → 等待到期 → 执行回调
```

本阶段暂不接入线程池，不实现周期任务和任务取消，先打通最基本的调度流程。

## 一、核心功能

### 1. 定时任务表示

定义 `TimerTask`，至少包含：

```cpp
struct TimerTask {
    TimerId id;
    TimePoint expiration;
    Task callback;
};
```

任务按照 `expiration` 排序。

### 2. 延迟任务提交

提供基础接口：

```cpp
TimerId schedule_after(Duration delay, Task task);
```

内部计算：

```cpp
expiration = steady_clock::now() + delay;
```

### 3. 调度器生命周期

提供：

```cpp
void start();
void stop();
```

要求：

* `start()` 启动一个调度线程
* `stop()` 唤醒并退出调度线程
* 析构时能够安全停止
* 禁止重复启动造成多个调度线程

### 4. 最近任务管理

内部使用最小堆：

```cpp
std::priority_queue<
    TimerTask,
    std::vector<TimerTask>,
    TimerTaskCompare
>
```

堆顶始终是最早到期的任务。

### 5. 调度线程循环

调度线程主要流程：

```text
任务队列为空
    ↓
等待新任务

任务队列非空
    ↓
等待堆顶任务到期

任务到期
    ↓
移除任务并执行回调
```

等待过程使用：

```cpp
condition_variable::wait()
condition_variable::wait_until()
```

### 6. 新任务抢占唤醒

当新加入的任务比当前堆顶更早到期时，需要唤醒调度线程，重新计算等待时间。

例如：

```text
原本等待 10 秒后的任务
    ↓
新增一个 1 秒后的任务
    ↓
必须立即唤醒调度线程
```

## 二、第一阶段建议接口

```cpp
class TimerScheduler {
public:
    using TimerId = std::uint64_t;
    using Task = MoveOnlyFunction<void()>;
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;

    TimerScheduler();
    ~TimerScheduler();

    void start();
    void stop();

    TimerId schedule_after(Duration delay, Task task);

private:
    void worker_loop();
};
```

这里可以直接复用线程池阶段实现的 `MoveOnlyFunction<void()>`。

## 三、第一阶段暂不实现

本阶段先不加入：

* 周期任务 `schedule_every`
* 任务取消 `cancel`
* 重新调度 `reschedule`
* 线程池投递
* 时间轮
* Cron 表达式
* 任务持久化
* 复杂停止策略

## 四、完成标准

第一阶段完成后，应通过以下测试：

1. 单个任务能够在指定延迟后执行。
2. 多个任务能够按照到期时间顺序执行。
3. 后加入的更早任务能够抢占原有等待。
4. 多线程能够安全提交定时任务。
5. 空任务队列时调度线程不会忙等。
6. `stop()` 能够及时唤醒并退出调度线程。
7. 任务回调抛出异常时，不会导致调度线程崩溃。

第一阶段的核心就是：

> **使用最小堆管理任务，通过条件变量等待最近到期时间，由一个调度线程执行到期任务。**
