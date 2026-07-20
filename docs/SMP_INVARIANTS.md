# SMP 不变量与锁顺序文档

## 1. 关键不变量

### I1: 任务唯一性
任意时刻每个 live task 只能处于以下位置之一：
- 一个 CPU 的 `_current_task[cpu]`（RUNNING）
- ready_list 的一个优先级链表（READY）
- 一个 wait_queue（BLOCKED）
- 不在任何队列（SUSPENDED/TERMINATED 等待回收）

**禁止同一 TCB 同时出现在两个队列或两个 CPU。**

### I2: ready_list 一致性
ready_bitmap 和 ready_list 必须一致：
- bitmap 有 bit → 对应优先级链表 head 非 NULL
- bitmap 无 bit → 对应优先级链表 head 为 NULL

### I3: tick 单调性
tick_count 单调递增，与 CPU 数无关（core0 是 timekeeper）。

### I4: 唤醒幂等性
任何 wakeup 最多完成一次。sched_wakeup 开头检查 state != BLOCKED 则直接返回。

### I5: 锁持有时 PendSV 不触发
持 sched_lock 的 C 代码路径必须在 **解锁后** 才调 `hal_trigger_pendsv()`。
PendSV handler 用 `spin_lock(&sched_lock.lock)`，如果持锁时触发 PendSV 会自旋死锁。

## 2. 全局锁顺序

```
mux_lock / sem_lock / mqueue_lock / event_lock / ep_lock / ch_lock
  ↓ (可以获取)
sched_lock
  ↓ (可以获取)
cap_pool_lock / task_lock / mem_lock / timer_lock / bh_lock
```

规则：
- 同层锁之间**禁止嵌套获取**（除非能证明不冲突）
- 跨层只能从上往下获取
- `sched_lock` 是中间层：IPC 对象锁可以获取它（如 mutex PI），task/cap/mem 锁不获取它

### 例外
- PendSV handler 用 `spin_lock(&sched_lock.lock)`（不操作 PRIMASK）
- 其他路径用 `irq_spin_lock(&sched_lock)`（先 PRIMASK 关中断再 spinlock）

## 3. 各子系统的锁

| 子系统 | 锁 | 类型 | 保护范围 |
|--------|-----|------|----------|
| scheduler | sched_lock | irq_spinlock_t (导出) | ready_list, ready_bitmap, sched_get_highest_ready |
| PendSV | sched_lock.lock | spinlock_t (内部) | PendSV handler 里操作 ready_list |
| capability | cap_pool_lock | irq_spinlock_t | cap_pool[] 所有写操作 |
| task | task_lock | irq_spinlock_t | task_pool[], task_used_bitmap |
| mem (heap) | mem_lock | irq_spinlock_t | mem_heap, free_list |
| semaphore | sem_lock | irq_spinlock_t | sem_pool, sem_used_bitmap |
| mutex | mux_lock | irq_spinlock_t | mutex_pool, wait_queue |
| mqueue | mqueue_lock | irq_spinlock_t | mqueue_pool, wait_queue |
| event | event_lock | irq_spinlock_t | event_pool, wait_queue |
| endpoint | ep_lock | irq_spinlock_t | ep_pool, ring buffer, reply binding |
| channel | ch_lock | irq_spinlock_t | ch_pool, SHM, wait_queue |
| timer | timer_lock | irq_spinlock_t | timer_pool, timer_used_bitmap |
| bh | bh_lock | irq_spinlock_t | bh_pool |
| trace | trace_slock | spinlock_t | trace_buf, trace_head, trace_count |
| stats | 原子操作 (ldrex/strex) | — | kern_stats 计数 |

## 4. per-CPU 数据（无需锁）

| 数据 | 说明 |
|------|------|
| `_current_task[cpu]` | 本核当前任务 |
| `_next_task[cpu]` | 本核下一个任务 |
| `need_resched[cpu]` | 本核重调度标志 |
| `idle_tasks[cpu]` | 本核 idle TCB |
| `idle_stacks[cpu]` | 本核 idle 栈 |

## 5. 已知限制（留后续里程碑）

- **无 IPI**：跨核唤醒靠 tick handler idle 旁路检查，延迟最多 1ms
- **无 CPU affinity**：所有任务可跑在任意核，无 cache locality 优化
- **全局 ready_list**：两核每次切任务都串行化（粗粒度但正确）
- **stats_task_switch 在 sched_unlock 外**：读 TCB 字段无锁（统计容错）
- **task_get_tcb 读路径无锁**：依赖 generation + volatile（M2 对象 generation 改进）
