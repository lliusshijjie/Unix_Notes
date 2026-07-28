# 定时器调度器：第二阶段实现框架

## 阶段目标

在第一阶段“一次性延迟任务”的基础上，补充完整的**任务管理能力**：

> 支持取消任务、周期任务，并正确处理任务状态和并发竞争。

---

## 一、增加任务取消

提供接口：

```cpp
bool cancel(TimerId id);
```

需要实现：

* 根据 `TimerId` 标记任务已取消
* 已取消任务不再执行
* 重复取消返回 `false`
* 不存在或已经执行完成的任务返回 `false`
* 支持其他线程并发取消任务

由于 `priority_queue` 不方便删除中间元素，建议采用：

```text
最小堆保存定时任务
+
哈希表保存任务状态
+
到达堆顶时惰性删除
```

---

## 二、增加周期任务

提供接口：

```cpp
TimerId schedule_every(Duration interval, Task task);
```

周期任务执行流程：

```text
任务到期
  ↓
执行回调
  ↓
计算下一次到期时间
  ↓
重新放入最小堆
```

第二阶段先采用**固定延迟语义**：

```text
本次执行完成时间 + interval = 下次到期时间
```

暂时不处理固定频率和追赶执行。

---

## 三、扩展任务结构

```cpp
struct TimerTask {
    TimerId id;
    TimePoint expiration;
    Duration interval;
    bool repeated;
    mutable Task callback;
};
```

任务状态可以单独维护：

```cpp
enum class TimerState {
    Pending,
    Cancelled,
    Executing
};
```

---

## 四、调整调度循环

`worker_loop()` 需要增加以下处理：

1. 清理堆顶已经取消的任务。
2. 一次性任务执行后彻底移除。
3. 周期任务执行后重新调度。
4. 周期任务执行期间被取消时，不再重新入堆。
5. 新任务或取消堆顶任务时，及时唤醒调度线程。

---

## 五、建议接口框架

```cpp
class TimerScheduler {
public:
    TimerId schedule_after(Duration delay, Task task);

    TimerId schedule_every(Duration interval, Task task);

    bool cancel(TimerId id);

    void start();
    void stop();

private:
    void worker_loop();
};
```

---

## 六、第二阶段暂不实现

暂时不加入：

* 线程池投递
* `reschedule`
* 固定频率周期任务
* 超时任务批量执行
* 时间轮
* 任务持久化
* Cron 表达式
* 统计监控

---

## 七、完成标准

第二阶段至少验证：

* 一次性任务可以成功取消。
* 已取消任务不会执行。
* 周期任务能够重复执行。
* 周期任务可以被取消。
* 多线程提交和取消不存在数据竞争。
* 取消当前最早任务后，调度线程能够重新等待。
* 周期回调抛出异常后，调度线程仍能继续运行。

第二阶段的核心可以概括为：

> **在基础时间调度之上，补充任务状态管理和周期执行能力。**
