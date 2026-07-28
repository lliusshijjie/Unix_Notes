# ThreadPool 知识清单

## 1. 架构总览

```text
外部线程 ──submit/post──► 全局有界环形队列
                              │
Worker i ◄──优先── 本地双端队列 ◄── Worker 内再提交
   │                    │
   │                    └── 尾部可被其他 Worker 窃取
   └── 空闲时：本地 → 全局 → 窃取 → 休眠
```

| 组件 | 职责 |
|---|---|
| `ThreadPool` | 固定 Worker、提交接口、关闭与唤醒 |
| `BoundedRingQueue` | 全局有界任务队列（mutex 保护） |
| `LocalTaskQueue` | 每 Worker 一个 deque：头给 Owner，尾给 Thief |
| `MoveOnlyFunction` | 只移不可拷的类型擦除任务包装（含 SBO） |

---

## 2. 提交语义

| 接口 | 行为 |
|---|---|
| `try_post` | 非阻塞：accepted / queue_full / pool_stopping |
| `post_wait` | 阻塞直到有空位或停止 |
| `post_for` | 限时等待，超时返回 `timeout` |
| `submit` | 包装 `packaged_task`，返回 `future` |

- Worker 线程内提交 → 进**本地队列**（不占全局容量）
- 外部线程提交 → 进**全局队列**
- 拒绝策略（仅全局满时）：`abort` / `caller_runs` / `discard`（锁外执行）

---

## 3. 重点优化

**MoveOnlyFunction + SBO**  
去掉 `shared_ptr<packaged_task>`；小对象放 64B 内联缓冲，大对象才堆分配。

**有界环形队列**  
固定缓冲、O(1) 入出队，避免 `std::queue` 节点分配；`push`/`pop` 显式检查满/空。

**工作窃取**  
Owner 头插头取（局部性好）；空闲 Worker 从别人尾部偷，均衡负载。思想来自 Cilk / ForkJoin。

**休眠与唤醒**  
无任务时 `wait`；有任务时 `notify_one`（避免惊群）；析构 `notify_all` 确保退出。

---

## 4. 必须守住的不变量

1. 取任务在锁内，执行在锁外  
2. 全局队列长度 ≤ capacity  
3. `caller_runs` / 用户回调不得持队列锁  
4. 关闭后：拒绝新任务，排空已入队任务（含本地队列）再退出  
5. CV 谓词同时检查停止条件与“是否还有可做的活”
