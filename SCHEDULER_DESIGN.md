# 调度器重写设计文档

## 1. 设计目标

- **目的**：教学/学习用途，代码清晰易懂
- **调度算法**：优先级抢占 + 时间片轮转
- **上下文切换**：PendSV 统一处理，调度器核心只修改就绪队列和当前任务指针，不直接触碰上下文
- **状态管理**：扩展状态机，状态明确
- **临界区保护**：基于 BasePri 的中断屏蔽（保留高优先级硬件中断响应）
- **优先级继承**：互斥锁支持优先级继承，避免优先级反转

---

## 2. 状态机设计

### 2.1 状态定义

| 状态 | 含义 |
|------|------|
| CREATED | 已创建，尚未启动 |
| READY | 就绪，等待 CPU |
| RUNNING | 正在执行 |
| BLOCKED | 阻塞，等待事件（锁/信号量/延时） |
| SUSPENDED | 挂起，主动暂停 |
| TERMINATED | 已终止，等待回收 |

### 2.2 状态转换图

```
                    task_start()
       ┌─────────┐ ──────────────► ┌─────────┐
       │ CREATED │                  │  READY  │◄──────────────────┐
       └─────────┘                  └────┬────┘                   │
            ▲                            │                        │
            │                     schedule()                      │
            │                        (被选中)                     │
            │                            │                        │
            │                            ▼                        │
            │                       ┌─────────┐                  │
            │             ┌────────│ RUNNING │─────────┐        │
            │             │        └────┬────┘         │        │
            │             │             │              │        │
            │  preempt()  │             │   block()   │        │
            │ (时间片/抢占)│             │ (等待资源)   │        │
            │             │             │              │        │
            │             │             ▼              ▼        │
            │             │        ┌─────────┐   ┌─────────┐    │
            │             └───────►│  READY  │   │ BLOCKED │────┤
            │                      └─────────┘   └────┬────┘    │
            │                          ▲     ▲        │         │
            │                          │     │        │         │
            │                          │     │ wakeup()         │
            │                      resume()  │   (资源可用)      │
            │                          │     │        │         │
            │                          │     └────────┘         │
            │                          │                        │
            │                     suspend()                     │
            │                          │                        │
            │                     ┌────┴────┐                   │
            └─────────────────────│SUSPENDED│◄──────────────────┘
                                  └─────────┘
                                        │
                                   resume()
                                        │
                                        └──────────────────────────┐
                                                                   │
                               exit()                             │
                          ┌────────────┐                          │
                          │ TERMINATED │◄─────────────────────────┘
                          └─────┬──────┘    (从 RUNNING/READY/BLOCKED)
                                │
                          task_wait() / 自动回收
                                │
                                ▼
                          [TCB 释放，可重新分配]
```

### 2.3 状态转换表

| 当前状态 | 事件/触发 | 新状态 | 调度器动作 |
|---------|----------|--------|-----------|
| CREATED | task_start() | READY | 加入就绪队列 |
| READY | schedule() | RUNNING | 从就绪队列移除，恢复上下文 |
| READY | task_suspend() | SUSPENDED | 从就绪队列移除 |
| RUNNING | preempt() | READY | 保存上下文，加入就绪队列尾部 |
| RUNNING | block() | BLOCKED | 保存上下文，加入等待队列 |
| RUNNING | task_suspend() | SUSPENDED | 保存上下文，不加入任何队列 |
| RUNNING | exit() | TERMINATED | 设置终止状态，触发调度 |
| BLOCKED | wakeup() | READY | 从等待队列移除，加入就绪队列 |
| BLOCKED | timeout() | READY | 超时唤醒，加入就绪队列 |
| BLOCKED | task_suspend() | SUSPENDED | 从等待队列移除 |
| SUSPENDED | resume() | READY | 加入就绪队列 |
| TERMINATED | task_wait() / 自动回收 | (释放) | TCB 归还池，栈标记空闲 |

---

## 3. 模块架构

### 3.1 分层架构

```
┌─────────────────────────────────────────────────────────────┐
│                      应用层 API                              │
│  task_create() task_delay() sem_wait() mutex_lock() ...    │
├─────────────────────────────────────────────────────────────┤
│                      调度器核心                              │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐         │
│  │ 状态管理    │  │ 就绪队列    │  │ 调度算法    │         │
│  │ (纯 C 代码) │  │ (纯 C 代码) │  │ (纯 C 代码) │         │
│  └─────────────┘  └─────────────┘  └─────────────┘         │
│                                                             │
│  职责：修改就绪队列、更新任务状态、选择下一任务              │
│  不负责：保存/恢复寄存器（由 PendSV 统一处理）               │
├─────────────────────────────────────────────────────────────┤
│                      上下文切换器                            │
│  ┌─────────────┐  ┌─────────────┐                          │
│  │ PendSV 汇编 │  │ SVC 汇编    │                          │
│  │ (保存/恢复) │  │ (首次切换)  │                          │
│  └─────────────┘  └─────────────┘                          │
│                                                             │
│  职责：保存当前任务 R4-R11 到其栈，恢复下一任务 R4-R11       │
│  触发方式：hal_trigger_pendsv() 设置 ICSR.PENDSVSET        │
├─────────────────────────────────────────────────────────────┤
│                      HAL 抽象层                              │
│  CPU 中断 栈 定时器                                         │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 上下文切换的分离式设计

**核心原则**：调度器核心（C 代码）只负责"决策"，PendSV（汇编）负责"执行"。

```
调用流程示例（task_delay）：

1. task_delay(10)
   └─► 2. sched_block(BLOCK_REASON_SLEEP, NULL, 10)
        │   - 设置 current->state = BLOCKED
        │   - 从就绪队列移除 current
        │   - 设置 current->wake_tick
        │   - 选择下一任务 next = ready_get_highest()
        │   - 设置 _next_task = next
        │
        └─► 3. hal_trigger_pendsv()
             │   - 设置 ICSR.PENDSVSET
             │   - 函数返回（不等待！）
             │
             └─► 4. PendSV_Handler（异常入口）
                  │   - 保存当前任务 R4-R11 到其栈
                  │   - 保存 PSP 到 current->sp
                  │   - 加载 next->sp 到 PSP
                  │   - 恢复 next 的 R4-R11
                  │   - 异常返回到 next 任务
```

**关键点**：
- `sched_block()` 函数返回后，当前任务停止执行
- PendSV 在函数返回后、下一条指令前执行
- 当前任务的寄存器状态在 PendSV 中保存，不会与 C 函数调用栈冲突

### 3.3 调度器核心职责

```
┌─────────────────────────────────────────────────────────────┐
│                     调度器核心职责                           │
├─────────────────────────────────────────────────────────────┤
│  1. TCB 管理                                                │
│     - 创建/销毁 TCB                                         │
│     - 分配/释放栈空间                                        │
│     - 初始化栈帧（调用 HAL）                                  │
├─────────────────────────────────────────────────────────────┤
│  2. 就绪队列管理                                            │
│     - 按优先级组织（位图 + 链表数组）                        │
│     - O(1) 插入/删除                                        │
│     - O(1) 查找最高优先级                                    │
├─────────────────────────────────────────────────────────────┤
│  3. 调度算法                                                │
│     - 优先级抢占：高优先级总是先运行                          │
│     - 时间片轮转：同优先级任务轮流执行                        │
│     - 空闲任务：无就绪任务时运行                              │
├─────────────────────────────────────────────────────────────┤
│  4. 抢占检查与触发                                          │
│     - 时钟中断：时间片耗尽                                   │
│     - 唤醒高优先级任务：立即触发抢占                          │
│     - 中断返回：检查 need_resched 标志                       │
├─────────────────────────────────────────────────────────────┤
│  5. 阻塞/唤醒                                              │
│     - block()：移出就绪队列，设置唤醒时间                     │
│     - wakeup()：加入就绪队列，检查是否需要抢占                │
│     - 超时处理：定时器到期自动唤醒                           │
├─────────────────────────────────────────────────────────────┤
│  6. 任务回收                                                │
│     - 终止任务进入 TERMINATED 状态                           │
│     - 自动回收或等待父任务回收                               │
└─────────────────────────────────────────────────────────────┘
```

---

## 4. 依赖关系

### 4.1 被调度器依赖的模块

| 模块 | 用途 |
|------|------|
| 系统调用 | 用户态进入内核态 |
| 中断处理 | 时钟中断、外设中断 |
| 同步原语 | 信号量、互斥锁（含优先级继承）、事件 |
| IPC | 消息队列、管道 |
| 定时器 | 软件定时器、延时 |
| 设备驱动 | I/O 完成唤醒 |

### 4.2 调度器依赖的底层

| 底层设施 | 用途 | 必要性 |
|---------|------|--------|
| 时钟中断 | 时间片、延时 | 必须 |
| BasePri 中断屏蔽 | 临界区保护 | 必须 |
| PendSV/SVC | 上下文切换 | 必须 |
| PSP/MSP | 任务栈隔离 | 必须 |

---

## 5. 临界区保护

### 5.1 基于 BasePri 的设计

**问题**：直接关总中断（cpsid i）会屏蔽所有中断，包括高优先级的硬件中断（如电机控制、通信接收），导致系统实时性下降。

**解决方案**：使用 BasePri 寄存器，只屏蔽调度相关的中断，保留高优先级硬件中断响应。

```
中断优先级布局（Cortex-M，数值越小优先级越高）：

┌─────────────────────────────────────────────────────┐
│  优先级 0-1：硬件关键中断（电机、通信）不屏蔽        │
├─────────────────────────────────────────────────────┤
│  优先级 2：调度器临界区阈值（BasePri）              │
├─────────────────────────────────────────────────────┤
│  优先级 3-14：普通中断                              │
├─────────────────────────────────────────────────────┤
│  优先级 15：SysTick、PendSV（最低，可被抢占）       │
└─────────────────────────────────────────────────────┘
```

### 5.2 临界区实现

```c
#define SCHED_CRITICAL_PRIORITY    2    // 调度器临界阈值
#define SYSTICK_PRIORITY           15   // SysTick 优先级（最低）
#define PENDSV_PRIORITY            15   // PendSV 优先级（最低）

// 进入临界区：提升 BasePri 到调度阈值
static inline uint32_t enter_critical(void) {
    uint32_t basepri;
    __asm volatile("mrs %0, basepri" : "=r"(basepri));
    __asm volatile("msr basepri, %0" :: "r"(SCHED_CRITICAL_PRIORITY << 4));
    __asm volatile("isb");
    return basepri;
}

// 退出临界区：恢复 BasePri
static inline void exit_critical(uint32_t basepri) {
    __asm volatile("msr basepri, %0" :: "r"(basepri));
}
```

**效果**：
- 优先级 0-1 的硬件中断可以打断临界区
- SysTick 和 PendSV（优先级 15）在临界区内被屏蔽
- 调度器数据结构受到保护

### 5.3 中断优先级配置

```c
void hal_interrupt_priority_init(void) {
    // 设置 SysTick 优先级为最低
    SCB->SHP[11] = SYSTICK_PRIORITY << 4;  // SysTick
    
    // 设置 PendSV 优先级为最低
    SCB->SHP[14] = PENDSV_PRIORITY << 4;   // PendSV
    
    // 设置 SVC 优先级为最高（用于首次切换）
    SCB->SHP[7] = 0;                        // SVC
}
```

---

## 6. 就绪队列实现

### 6.1 数据结构

```c
// 每个优先级一个双向链表
typedef struct {
    tcb_t *head;
    tcb_t *tail;
} ready_list_t;

// 就绪队列数据
static struct {
    ready_list_t lists[KERN_MAX_PRIORITY];  // 每优先级一个链表
    uint32_t bitmap[4];                      // 优先级位图（128 位）
} ready_queue;

// 位图操作
static inline void bitmap_set(uint8_t prio) {
    ready_queue.bitmap[prio / 32] |= (1U << (prio % 32));
}

static inline void bitmap_clear(uint8_t prio) {
    ready_queue.bitmap[prio / 32] &= ~(1U << (prio % 32));
}

static inline int find_highest_priority(void) {
    for (int i = 0; i < 4; i++) {
        if (ready_queue.bitmap[i] != 0) {
            return i * 32 + __builtin_ctz(ready_queue.bitmap[i]);
        }
    }
    return -1;  // 无就绪任务
}
```

### 6.2 操作复杂度

| 操作 | 复杂度 | 说明 |
|------|--------|------|
| ready_add(tcb) | O(1) | 插入对应优先级链表尾部，设置位图 |
| ready_remove(tcb) | O(1) | 从链表移除，若链表空则清除位图 |
| ready_get_highest() | O(1) | 位图找最高优先级，返回链表头 |

---

## 7. 时间片中断处理

### 7.1 SysTick 中断流程

```
SysTick_Handler
      │
      ▼
┌─────────────────────────────────┐
│  1. 递增 tick_count             │
└──────────────┬──────────────────┘
               ▼
┌─────────────────────────────────┐
│  2. 检查当前任务有效性          │
│     current == NULL ? return    │
│     current == idle ? 跳过时间片│
└──────────────┬──────────────────┘
               ▼
┌─────────────────────────────────┐
│  3. 当前任务时间片递减           │
│     time_slice--                │
└──────────────┬──────────────────┘
               ▼
┌─────────────────────────────────┐
│  4. 时间片耗尽？                 │
│     time_slice == 0 ?           │
└──────────────┬──────────────────┘
               │
        ┌──────┴──────┐
        │ 是          │ 否
        ▼             ▼
┌───────────────┐   ┌─────────────────────────────┐
│ 设置重调度标志 │   │ 5. 检查超时唤醒              │
│ need_resched  │   │    遍历所有阻塞任务          │
│ 触发 PendSV   │   │    wake_tick <= tick_count ? │
└───────────────┘   └──────────────┬──────────────┘
                                   ▼
                          ┌────────────────────────┐
                          │ 6. 有任务需要唤醒？      │
                          └────────────┬───────────┘
                                       │
                                ┌──────┴──────┐
                                │ 是          │ 否
                                ▼             ▼
                        ┌───────────────┐   ┌────────┐
                        │ sched_wakeup()│   │ 返回   │
                        │ (可能触发抢占) │   └────────┘
                        └───────────────┘
```

### 7.2 时间片处理代码

```c
void sched_tick_handler(void) {
    scheduler.tick_count++;
    
    tcb_t *current = scheduler.current_task;
    
    // 空闲任务不处理时间片
    if (current == NULL || current->id < 0) {
        goto check_timeout;
    }
    
    // 时间片递减
    if (current->time_slice > 0) {
        current->time_slice--;
        
        if (current->time_slice == 0) {
            scheduler.need_resched = 1;
            hal_trigger_pendsv();
        }
    }
    
check_timeout:
    // 检查超时唤醒
    check_timeout_wakeup();
}
```

### 7.3 超时检查性能说明

**教学简化版**：遍历所有阻塞任务检查超时

```c
// O(n) 遍历，教学目的简化实现
static void check_timeout_wakeup(void) {
    for (task_id_t id = 0; id < KERN_MAX_TASKS; id++) {
        tcb_t *tcb = &task_pool[id];
        if (tcb->state == TASK_STATE_BLOCKED &&
            tcb->wake_tick > 0 &&
            tcb->wake_tick <= scheduler.tick_count) {
            sched_wakeup(tcb, KERN_ERR_TIMEOUT);
        }
    }
}
```

**生产级优化方案**：
1. 按唤醒时间排序的定时器链表
2. 时间轮（Timing Wheel）
3. 最小堆

### 7.4 抢占检查点

| 检查点 | 触发条件 | 动作 |
|-------|---------|------|
| SysTick 中断 | 时间片耗尽（非空闲任务） | 设置 need_resched，触发 PendSV |
| SysTick 中断 | 有任务超时唤醒 | 调用 sched_wakeup()，内部检查抢占 |
| sched_wakeup() | 唤醒的任务优先级高于当前 | 触发 PendSV |
| sched_add_ready() | 新任务优先级高于当前 | 触发 PendSV |
| task_yield() | 主动调用 | 触发 PendSV |
| mutex_unlock() | 有高优先级等待者 | 触发 PendSV |

### 7.5 唤醒时的抢占检查

```c
void sched_wakeup(tcb_t *tcb, kern_err_t result) {
    uint32_t crit = enter_critical();
    
    if (tcb->state != TASK_STATE_BLOCKED) {
        exit_critical(crit);
        return;
    }
    
    // 更新任务状态
    tcb->state = TASK_STATE_READY;
    tcb->block_result = result;
    tcb->wake_tick = 0;
    
    // 加入就绪队列
    ready_add(tcb);
    
    // 关键：检查是否需要抢占
    tcb_t *current = scheduler.current_task;
    if (current && tcb->priority < current->priority) {
        // 唤醒的任务优先级更高，触发抢占
        hal_trigger_pendsv();
    }
    
    exit_critical(crit);
}
```

---

## 8. 优先级继承

### 8.1 优先级反转问题

```
场景：
- 低优先级任务 L 持有互斥锁
- 中优先级任务 M 抢占了 L
- 高优先级任务 H 尝试获取锁被阻塞

结果：M 一直运行，H 无法运行（因为 L 无法运行来释放锁）
```

### 8.2 优先级继承机制

```c
// 互斥锁结构
typedef struct {
    task_id_t owner;              // 持有者
    uint8_t lock_count;           // 锁计数（递归锁）
    uint8_t owner_original_prio;  // 持有者原始优先级
    wait_queue_t wait_queue;      // 等待队列
} mutex_t;

// 获取互斥锁
kern_err_t mutex_lock(mutex_id_t id) {
    mutex_t *mutex = &mutex_pool[id];
    tcb_t *current = sched_get_current();
    
    enter_critical();
    
    if (mutex->owner == current->id) {
        // 递归锁
        mutex->lock_count++;
        exit_critical();
        return KERN_OK;
    }
    
    if (mutex->owner == -1) {
        // 锁空闲，直接获取
        mutex->owner = current->id;
        mutex->lock_count = 1;
        mutex->owner_original_prio = current->priority;
        exit_critical();
        return KERN_OK;
    }
    
    // 锁被占用，检查优先级继承
    tcb_t *owner = &task_pool[mutex->owner];
    if (current->priority < owner->priority) {
        // 当前任务优先级更高，提升持有者优先级
        owner->priority = current->priority;
        // 如果持有者在就绪队列，需要重新插入
        if (owner->state == TASK_STATE_READY) {
            ready_remove(owner);
            ready_add(owner);
        }
    }
    
    // 阻塞等待
    exit_critical();
    return sched_block(BLOCK_REASON_MUTEX, mutex, 0);
}

// 释放互斥锁
kern_err_t mutex_unlock(mutex_id_t id) {
    mutex_t *mutex = &mutex_pool[id];
    tcb_t *current = sched_get_current();
    
    enter_critical();
    
    if (mutex->owner != current->id) {
        exit_critical();
        return KERN_ERR_STATE;
    }
    
    mutex->lock_count--;
    if (mutex->lock_count > 0) {
        exit_critical();
        return KERN_OK;
    }
    
    // 恢复原始优先级
    current->priority = mutex->owner_original_prio;
    
    // 唤醒等待者
    tcb_t *waiter = wait_queue_pop(&mutex->wait_queue);
    if (waiter) {
        mutex->owner = waiter->id;
        mutex->lock_count = 1;
        mutex->owner_original_prio = waiter->priority;
        sched_wakeup(waiter, KERN_OK);
    } else {
        mutex->owner = -1;
    }
    
    exit_critical();
    return KERN_OK;
}
```

---

## 9. 任务终止与回收

### 9.1 任务退出流程

```c
void task_exit(void *retval) {
    (void)retval;
    
    tcb_t *current = sched_get_current();
    
    // 设置终止状态
    current->state = TASK_STATE_TERMINATED;
    
    // 释放任务 ID（标记可回收）
    // 注意：TCB 和栈在回收时才释放
    
    // 触发调度，切换到下一任务
    sched_yield();
    
    // 不应该到达这里
    while (1);
}
```

### 9.2 任务回收机制

**方案 B：自动回收（推荐）**

```c
// 在 PendSV 处理中检查并回收终止的任务
void kern_pendsv_handler(void) {
    tcb_t *current = _current_task;
    
    // 处理当前任务状态转换
    if (current) {
        switch (current->state) {
        case TASK_STATE_RUNNING:
            // 时间片轮转
            current->state = TASK_STATE_READY;
            current->time_slice = current->time_slice_reload;
            ready_add(current);
            break;
            
        case TASK_STATE_TERMINATED:
            // 自动回收
            task_reclaim(current);
            break;
            
        case TASK_STATE_BLOCKED:
        case TASK_STATE_SUSPENDED:
            // 不加入就绪队列
            break;
        }
    }
    
    // 选择下一任务
    tcb_t *next = ready_get_highest();
    if (next == NULL) {
        next = &idle_task;
    } else {
        ready_remove(next);
    }
    
    next->state = TASK_STATE_RUNNING;
    _next_task = next;
    scheduler.current_task = next;
}

// 回收任务资源
static void task_reclaim(tcb_t *tcb) {
    if (tcb->id < 0) return;  // 空闲任务不回收
    
    // 释放任务 ID
    task_used_bitmap &= ~(1U << tcb->id);
    
    // 清零 TCB（可选，调试时保留）
    // memset(tcb, 0, sizeof(tcb_t));
    
    // 栈空间标记空闲（静态池，实际无需操作）
}
```

**方案 A：等待回收（类 Linux wait）**

```c
// 父任务等待子任务结束
kern_err_t task_wait(task_id_t child_id, void **retval, uint32_t timeout) {
    tcb_t *child = &task_pool[child_id];
    
    while (child->state != TASK_STATE_TERMINATED) {
        if (timeout == 0) {
            return sched_block(BLOCK_REASON_JOIN, child, 0);
        } else {
            return sched_block(BLOCK_REASON_JOIN, child, timeout);
        }
    }
    
    // 回收资源
    task_reclaim(child);
    
    return KERN_OK;
}
```

---

## 10. 栈溢出检测

### 10.1 栈初始化时填充魔数

```c
#define STACK_MAGIC_BYTE    0xCC

void *hal_stack_init(void *stack_top, uint32_t stack_size,
                     void *entry, void *arg, void *exit) {
    uint8_t *stack = (uint8_t *)stack_top - stack_size;
    
    // 填充魔数（栈底区域）
    for (int i = 0; i < 16; i++) {
        stack[i] = STACK_MAGIC_BYTE;
    }
    
    // 初始化栈帧...
    // ...
    
    return sp;
}
```

### 10.2 栈溢出检查

```c
// 在空闲任务中定期检查
static void check_stack_overflow(void) {
    for (task_id_t id = 0; id < KERN_MAX_TASKS; id++) {
        tcb_t *tcb = &task_pool[id];
        if (tcb->state == TASK_STATE_TERMINATED) continue;
        
        uint8_t *stack_base = (uint8_t *)tcb->stack_base;
        
        // 检查栈底魔数是否被破坏
        for (int i = 0; i < 16; i++) {
            if (stack_base[i] != STACK_MAGIC_BYTE) {
                // 栈溢出！
                hal_debug_puts("STACK OVERFLOW: task ");
                hal_debug_putc('0' + id);
                hal_debug_puts("\r\n");
                // 可选：挂起该任务
            }
        }
    }
}

// 空闲任务
static void idle_task_func(void *arg) {
    (void)arg;
    uint32_t check_counter = 0;
    
    while (1) {
        __asm volatile("wfi");
        
        // 每 1000 次循环检查一次栈溢出
        if (++check_counter >= 1000) {
            check_counter = 0;
            check_stack_overflow();
        }
    }
}
```

---

## 11. 空闲任务设计

### 11.1 空闲任务特性

- **优先级**：最低（KERN_MAX_PRIORITY - 1）
- **状态**：永不阻塞，始终就绪
- **功能**：执行 WFI 指令进入低功耗，定期检查栈溢出

### 11.2 空闲任务实现

```c
// 空闲任务 TCB（静态分配）
static tcb_t idle_task;

// 空闲任务栈（静态分配）
static uint8_t idle_stack[IDLE_STACK_SIZE] __attribute__((aligned(8)));

// 空闲任务函数
static void idle_task_func(void *arg) {
    (void)arg;
    uint32_t check_counter = 0;
    
    while (1) {
        __asm volatile("wfi");
        
        // 定期检查栈溢出
        if (++check_counter >= 1000) {
            check_counter = 0;
            check_stack_overflow();
        }
    }
}

// 初始化空闲任务
void idle_task_init(void) {
    idle_task.id = -1;  // 特殊 ID
    idle_task.priority = KERN_MAX_PRIORITY - 1;
    idle_task.state = TASK_STATE_READY;
    idle_task.stack_base = idle_stack;
    idle_task.stack_size = IDLE_STACK_SIZE;
    idle_task.sp = hal_stack_init(
        idle_stack + IDLE_STACK_SIZE,
        IDLE_STACK_SIZE,
        idle_task_func,
        NULL,
        NULL  // 空闲任务不退出
    );
}
```

---

## 12. 静态内存管理

### 12.1 TCB 池

```c
// TCB 静态池
static tcb_t task_pool[KERN_MAX_TASKS];

// 任务使用位图
static uint32_t task_used_bitmap;

// 分配 TCB
static task_id_t alloc_task_id(void) {
    for (int i = 0; i < KERN_MAX_TASKS; i++) {
        if (!(task_used_bitmap & (1U << i))) {
            task_used_bitmap |= (1U << i);
            return (task_id_t)i;
        }
    }
    return KERN_INVALID_ID;
}

// 释放 TCB
static void free_task_id(task_id_t id) {
    if (id >= 0 && id < KERN_MAX_TASKS) {
        task_used_bitmap &= ~(1U << id);
    }
}
```

### 12.2 栈池

```c
// 栈池：每个任务独立栈，固定大小
static uint8_t task_stacks[KERN_MAX_TASKS][KERN_DEFAULT_STACK_SIZE]
    __attribute__((aligned(8)));

// 任务创建时分配栈
tcb->stack_base = task_stacks[id];
tcb->stack_size = KERN_DEFAULT_STACK_SIZE;
```

**说明**：
- 栈池为二维数组，每个任务有独立的栈空间
- 栈大小在编译时固定（KERN_DEFAULT_STACK_SIZE）
- 不支持动态栈大小分配（教学简化）
- TCB 中保存栈基址和栈指针

---

## 13. 文件结构

```
src/
├── kernel/
│   ├── core/
│   │   ├── scheduler.c      # 调度器核心逻辑
│   │   ├── scheduler.h      # 调度器接口
│   │   └── context.S        # 上下文切换（汇编）
│   ├── task/
│   │   ├── task.c           # 任务管理
│   │   └── task.h           # 任务接口
│   └── include/
│       ├── kernel_types.h   # TCB、状态等类型定义
│       └── kernel_config.h  # 配置参数
├── arch/arm/cortex-m7/
│   └── hal.c                # HAL 抽象层
└── hal/
    └── hal.h                # HAL 接口
```

---

## 14. 实现步骤

### 第一阶段：基础框架

| Step | 内容 | 文件 |
|------|------|------|
| 1 | 类型定义：任务状态枚举、TCB 结构体、错误码 | kernel_types.h |
| 2 | 配置参数：最大任务数、优先级数量、栈大小 | kernel_config.h |

### 第二阶段：HAL 层

| Step | 内容 | 文件 |
|------|------|------|
| 3 | HAL 接口定义：中断控制、时钟、上下文、调试 | hal.h |
| 4 | HAL 实现：Cortex-M7 具体实现、SysTick 配置 | hal.c |
| 5 | 中断优先级配置：BasePri、SysTick、PendSV 优先级 | hal.c |
| 6 | SysTick 中断：SysTick_Handler、kern_tick_handler | hal.c |
| 7 | 上下文切换汇编：PendSV_Handler、SVC_Handler | context.S |

### 第三阶段：调度器核心

| Step | 内容 | 文件 |
|------|------|------|
| 8 | 空闲任务：创建、初始化、栈溢出检查 | scheduler.c |
| 9 | TCB 和栈管理：静态 TCB 池、栈池、task_create/start | task.c |
| 10 | 就绪队列：位图 + 链表数组、ready_add/remove/get_highest | scheduler.c |
| 11 | 调度器核心：sched_init/start/yield、_current_task/_next_task | scheduler.c |
| 12 | PendSV 处理：kern_pendsv_handler、状态转换、任务回收 | scheduler.c |

### 第四阶段：阻塞机制

| Step | 内容 | 文件 |
|------|------|------|
| 13 | 阻塞/唤醒：sched_block、sched_wakeup（含抢占检查） | scheduler.c |
| 14 | 延时：task_delay、超时检查 | scheduler.c |
| 15 | 挂起/恢复：task_suspend、task_resume | task.c |
| 16 | 任务退出：task_exit、task_reclaim | task.c |

### 第五阶段：同步原语

| Step | 内容 | 文件 |
|------|------|------|
| 17 | 互斥锁：mutex_lock/unlock、优先级继承 | mutex.c |

### 第六阶段：测试验证

| Step | 内容 | 文件 |
|------|------|------|
| 18 | 基础测试：任务创建、优先级抢占、时间片轮转 | main.c |
| 19 | 阻塞测试：任务延时、挂起/恢复 | main.c |
| 20 | 优先级反转测试：验证优先级继承 | main.c |

---

## 15. 关键设计决策

| 决策点 | 选择 | 原因 |
|-------|------|------|
| 上下文切换 | PendSV 统一处理 | 调度器核心只修改状态和队列，不触碰寄存器 |
| 临界区保护 | BasePri 中断屏蔽 | 保留高优先级硬件中断响应，提高实时性 |
| 就绪队列 | 位图 + 链表数组 | O(1) 插入/删除/查找最高优先级 |
| 状态管理 | 扩展状态机 | 状态明确，易于理解 |
| 栈初始化 | HAL 层 | 便于移植 |
| 超时检查 | 遍历所有任务 | 教学简化，生产级需优化 |
| 优先级继承 | 支持 | 避免优先级反转问题 |
| 任务回收 | 自动回收 | 简化使用，避免内存泄漏 |
| 栈溢出检测 | 魔数填充 + 定期检查 | 教学调试辅助 |

---

## 16. 接口定义

### 16.1 任务管理接口

```c
/**
 * 创建任务
 * @param name       任务名称
 * @param entry      任务入口函数
 * @param arg        任务参数
 * @param priority   优先级（0 最高）
 * @param stack_size 栈大小（字节），必须为 0 或 KERN_DEFAULT_STACK_SIZE
 * @return 任务 ID，失败返回 KERN_INVALID_ID
 */
task_id_t task_create(const char *name, task_func_t entry, void *arg, 
                      uint8_t priority, uint32_t stack_size);

/**
 * 启动任务
 * @param task_id 任务 ID
 * @return KERN_OK 成功，其他失败
 */
kern_err_t task_start(task_id_t task_id);

/**
 * 任务退出
 * @param retval 返回值（未使用）
 */
void task_exit(void *retval);

/**
 * 挂起任务
 * @param task_id 任务 ID
 * @return KERN_OK 成功，其他失败
 */
kern_err_t task_suspend(task_id_t task_id);

/**
 * 恢复任务
 * @param task_id 任务 ID
 * @return KERN_OK 成功，其他失败
 */
kern_err_t task_resume(task_id_t task_id);

/**
 * 任务延时
 * @param ticks 延时时间（系统 tick 数）
 * @return KERN_OK 成功，KERN_ERR_TIMEOUT 超时
 */
kern_err_t task_delay(uint32_t ticks);

/**
 * 主动让出 CPU
 * @return KERN_OK
 */
kern_err_t task_yield(void);

/**
 * 获取当前任务 ID
 * @return 任务 ID
 */
task_id_t task_self(void);
```

### 16.2 调度器接口

```c
/**
 * 初始化调度器
 */
void sched_init(void);

/**
 * 启动调度器（不返回）
 */
void sched_start(void);

/**
 * 主动调度
 */
void sched_yield(void);

/**
 * 阻塞当前任务
 * @param reason  阻塞原因
 * @param obj     阻塞对象（信号量/互斥锁等）
 * @param timeout 超时时间（ticks），0 表示无限等待
 * @return KERN_OK 被正常唤醒，KERN_ERR_TIMEOUT 超时
 */
kern_err_t sched_block(block_reason_t reason, void *obj, uint32_t timeout);

/**
 * 唤醒任务
 * @param tcb    任务 TCB
 * @param result 唤醒结果
 */
void sched_wakeup(tcb_t *tcb, kern_err_t result);

/**
 * 添加到就绪队列
 * @param tcb 任务 TCB
 */
void sched_add_ready(tcb_t *tcb);

/**
 * 从就绪队列移除
 * @param tcb 任务 TCB
 */
void sched_remove_ready(tcb_t *tcb);

/**
 * 获取当前任务 TCB
 * @return 当前任务 TCB
 */
tcb_t *sched_get_current(void);

/**
 * 时钟中断处理
 */
void sched_tick_handler(void);
```

### 16.3 HAL 接口

```c
/**
 * 保存中断状态并提升 BasePri
 * @return BasePri 值
 */
uint32_t hal_irq_save(void);

/**
 * 恢复中断状态
 * @param basepri BasePri 值
 */
void hal_irq_restore(uint32_t basepri);

/**
 * 开中断（设置 BasePri = 0）
 */
void hal_irq_enable(void);

/**
 * 关中断（设置 BasePri = 0xFF）
 */
void hal_irq_disable(void);

/**
 * 初始化 SysTick
 * @param rate_hz 中断频率（Hz）
 */
void hal_systick_init(uint32_t rate_hz);

/**
 * 获取当前 tick
 * @return tick 计数
 */
uint32_t hal_systick_get(void);

/**
 * 初始化任务栈
 * @param stack_top  栈顶地址
 * @param stack_size 栈大小（字节），必须等于 KERN_DEFAULT_STACK_SIZE
 * @param entry      任务入口
 * @param arg        任务参数
 * @param exit       退出处理函数
 * @return 初始化后的栈指针
 */
void *hal_stack_init(void *stack_top, uint32_t stack_size, 
                     void *entry, void *arg, void *exit);

/**
 * 触发 PendSV
 */
void hal_trigger_pendsv(void);

/**
 * 触发首次切换（SVC）
 */
void hal_trigger_first_switch(void);

/**
 * 调试输出字符串
 * @param s 字符串
 */
void hal_debug_puts(const char *s);

/**
 * 调试输出字符
 * @param c 字符
 */
void hal_debug_putc(char c);
```

---

## 17. 预计工作量

| 阶段 | 文件数 | 预计时间 |
|------|-------|---------|
| 第一阶段：基础框架 | 2 | 20 分钟 |
| 第二阶段：HAL 层 | 3 | 50 分钟 |
| 第三阶段：调度器核心 | 2 | 70 分钟 |
| 第四阶段：阻塞机制 | 1 | 40 分钟 |
| 第五阶段：同步原语 | 1 | 30 分钟 |
| 第六阶段：测试验证 | 1 | 40 分钟 |
| **总计** | **10** | **约 4 小时** |

---

## 18. 首次调度流程

### 18.1 SVC 与 PendSV 的区别

| 特性 | SVC (首次切换) | PendSV (常规切换) |
|------|---------------|-------------------|
| 触发时机 | 调度器启动时 | 每次任务切换 |
| 当前任务 | `_current_task = NULL` | `_current_task` 有效 |
| 上下文保存 | 不需要（无当前任务） | 保存 R4-R11 到当前任务栈 |
| CONTROL 寄存器 | 设置 SPSEL=1 使用 PSP | 已设置，无需修改 |
| 返回地址 | 使用异常返回机制 | 使用异常返回机制 |

### 18.2 首次调度流程图

```
sched_start()
      │
      ▼
┌─────────────────────────────────────┐
│ 1. 检查就绪队列是否有任务            │
│    ready_bitmap == 0 ? 死循环等待    │
└──────────────────┬──────────────────┘
                   ▼
┌─────────────────────────────────────┐
│ 2. 选择第一个任务                    │
│    first = ready_get_highest()      │
│    ready_remove(first)              │
└──────────────────┬──────────────────┘
                   ▼
┌─────────────────────────────────────┐
│ 3. 设置任务状态                      │
│    first->state = RUNNING           │
│    _current_task = NULL (关键!)     │
│    _next_task = first               │
└──────────────────┬──────────────────┘
                   ▼
┌─────────────────────────────────────┐
│ 4. 初始化中断优先级                  │
│    SysTick = 最低 (15)              │
│    PendSV = 最低 (15)               │
│    SVC = 最高 (0)                   │
└──────────────────┬──────────────────┘
                   ▼
┌─────────────────────────────────────┐
│ 5. 启动 SysTick                      │
│    hal_systick_init(TICK_RATE)      │
└──────────────────┬──────────────────┘
                   ▼
┌─────────────────────────────────────┐
│ 6. 使能全局中断                      │
│    hal_irq_enable()                 │
└──────────────────┬──────────────────┘
                   ▼
┌─────────────────────────────────────┐
│ 7. 触发首次切换                      │
│    hal_trigger_first_switch()       │
│    (执行 SVC #0)                    │
└──────────────────┬──────────────────┘
                   ▼
            ┌──────────────┐
            │ SVC_Handler   │
            └──────┬───────┘
                   ▼
┌─────────────────────────────────────┐
│ 8. 恢复第一个任务上下文              │
│    ldmia sp!, {r4-r11}              │
│    msr psp, sp                      │
│    设置 CONTROL.SPSEL = 1           │
│    _current_task = _next_task       │
└──────────────────┬──────────────────┘
                   ▼
┌─────────────────────────────────────┐
│ 9. 异常返回到第一个任务              │
│    bx lr (LR = 0xFFFFFFFD)          │
└─────────────────────────────────────┘
```

### 18.3 SVC_Handler 汇编实现

```asm
SVC_Handler:
    // 获取下一个任务
    ldr     r0, =_next_task
    ldr     r0, [r0]
    ldr     r1, [r0]            // r1 = SP

    // 恢复 R4-R11
    ldmia   r1!, {r4-r11}

    // 设置 PSP
    msr     psp, r1

    // 设置 CONTROL 寄存器使用 PSP
    movs    r2, #2
    msr     control, r2
    isb

    // 更新 _current_task
    ldr     r2, =_current_task
    str     r0, [r2]

    // 使能中断
    cpsie   i

    // 异常返回到线程模式
    ldr     lr, =0xFFFFFFFD
    bx      lr
```

---

## 19. 中断返回抢占检查

### 19.1 问题背景

当 ISR 唤醒高优先级任务时，需要在中断返回后立即调度。但 Cortex-M 的异常返回机制会自动恢复上下文，如果不处理，会返回到被抢占的低优先级任务。

### 19.2 解决方案：PendSV 延迟调度

**核心思想**：在 ISR 中不直接切换任务，而是设置 PendSV 位，让 PendSV 在所有 ISR 完成后执行。

```
ISR (如 SysTick)
      │
      ▼
┌─────────────────────────────────────┐
│ 1. ISR 正常执行                      │
│    tick_count++                     │
│    检查超时唤醒                      │
└──────────────────┬──────────────────┘
                   ▼
┌─────────────────────────────────────┐
│ 2. 发现需要调度                      │
│    sched_wakeup() 发现高优先级任务   │
│    设置 need_resched = 1            │
│    hal_trigger_pendsv()             │
└──────────────────┬──────────────────┘
                   ▼
┌─────────────────────────────────────┐
│ 3. ISR 返回                          │
│    硬件恢复被中断任务的上下文        │
│    (注意：此时还未真正切换任务)      │
└──────────────────┬──────────────────┘
                   ▼
┌─────────────────────────────────────┐
│ 4. PendSV 立即触发                   │
│    (优先级最低，在 ISR 返回后执行)   │
│    保存当前任务上下文                │
│    切换到高优先级任务                │
└─────────────────────────────────────┘
```

### 19.3 中断嵌套场景

```
优先级 0: 硬件关键中断 (电机控制)
优先级 1: 通信接收中断
优先级 2: [临界区阈值]
优先级 3-14: 普通中断
优先级 15: SysTick, PendSV

场景：SysTick 中断中唤醒了高优先级任务

时间线：
┌─────────────────────────────────────────────────────────┐
│ 任务 A (低优先级) 正在运行                               │
├─────────────────────────────────────────────────────────┤
│ SysTick 中断 (优先级 15)                                 │
│   └─► sched_wakeup(高优先级任务 B)                      │
│   └─► hal_trigger_pendsv()                              │
│   └─► ISR 返回                                          │
├─────────────────────────────────────────────────────────┤
│ PendSV (优先级 15) 立即触发                              │
│   └─► 保存任务 A 上下文                                  │
│   └─► 切换到任务 B                                      │
├─────────────────────────────────────────────────────────┤
│ 任务 B (高优先级) 开始运行                               │
└─────────────────────────────────────────────────────────┘
```

### 19.4 临界区内的抢占处理

```c
// 如果在临界区内（BasePri >= SCHED_CRITICAL_PRIORITY）
// PendSV 会被屏蔽，直到退出临界区

void some_function(void) {
    uint32_t crit = enter_critical();  // BasePri = 2
    
    // ... 临界区代码 ...
    
    // 在临界区内唤醒高优先级任务
    sched_wakeup(high_prio_task);  // 设置 PendSV，但不会立即执行
    
    exit_critical(crit);  // 恢复 BasePri = 0
    // 此时 PendSV 立即触发！
}
```

---

## 20. 多任务同时唤醒

### 20.1 场景描述

多个任务因超时同时到期，需要在一个 tick 内全部唤醒。

### 20.2 处理策略

```c
void sched_tick_handler(void) {
    scheduler.tick_count++;
    
    // 检查所有阻塞任务
    for (task_id_t id = 0; id < KERN_MAX_TASKS; id++) {
        tcb_t *tcb = &task_pool[id];
        
        if (tcb->state == TASK_STATE_BLOCKED &&
            tcb->wake_tick > 0 &&
            tcb->wake_tick <= scheduler.tick_count) {
            
            // 唤醒任务（会加入就绪队列）
            sched_wakeup(tcb, KERN_ERR_TIMEOUT);
        }
    }
    
    // 注意：sched_wakeup 内部会检查是否需要抢占
    // 多次调用 sched_wakeup 会多次设置 PendSV
    // 但 PendSV 只执行一次，处理所有状态变化
}
```

### 20.3 唤醒顺序

**教学简化版**：按任务 ID 顺序唤醒

**生产级优化**：按优先级从高到低唤醒

```c
// 生产级：先收集需要唤醒的任务，按优先级排序后唤醒
static void check_timeout_wakeup_optimized(void) {
    tcb_t *to_wake[KERN_MAX_TASKS];
    int count = 0;
    
    // 收集
    for (task_id_t id = 0; id < KERN_MAX_TASKS; id++) {
        tcb_t *tcb = &task_pool[id];
        if (tcb->state == TASK_STATE_BLOCKED &&
            tcb->wake_tick > 0 &&
            tcb->wake_tick <= scheduler.tick_count) {
            to_wake[count++] = tcb;
        }
    }
    
    // 按优先级排序（插入排序，教学简化）
    for (int i = 1; i < count; i++) {
        tcb_t *key = to_wake[i];
        int j = i - 1;
        while (j >= 0 && to_wake[j]->priority > key->priority) {
            to_wake[j + 1] = to_wake[j];
            j--;
        }
        to_wake[j + 1] = key;
    }
    
    // 按优先级从高到低唤醒
    for (int i = 0; i < count; i++) {
        sched_wakeup(to_wake[i], KERN_ERR_TIMEOUT);
    }
}
```

### 20.4 抢占检查优化

```c
// 优化：只在最后一个唤醒后检查抢占
void sched_wakeup_batch(tcb_t *tcb, kern_err_t result, int is_last) {
    // ... 唤醒逻辑 ...
    
    if (is_last) {
        // 只在最后一个任务唤醒后检查抢占
        tcb_t *current = scheduler.current_task;
        tcb_t *highest = ready_get_highest();
        
        if (highest && highest->priority < current->priority) {
            hal_trigger_pendsv();
        }
    }
}
```

---

## 21. 空闲任务状态处理

### 21.1 空闲任务的特殊性

| 特性 | 说明 |
|------|------|
| ID | -1 (特殊值，不在任务池中) |
| 优先级 | KERN_MAX_PRIORITY - 1 (最低) |
| 状态 | 始终为 READY/RUNNING，永不阻塞 |
| 时间片 | 不参与时间片轮转 |
| 栈溢出检查 | 定期执行 |

### 21.2 空闲任务状态机

```
                    sched_start()
       ┌─────────┐ ──────────────► ┌─────────┐
       │ CREATED │                  │  READY  │
       └─────────┘                  └────┬────┘
            ▲                            │
            │                     无其他任务时
            │                     被选中运行
            │                            │
            │                            ▼
            │                       ┌─────────┐
            │             ┌────────│ RUNNING │◄────────┐
            │             │        └────┬────┘         │
            │             │             │              │
            │             │   有高优先级任务就绪       │
            │             │   (被抢占)                │
            │             │             │              │
            │             │             ▼              │
            │             │        ┌─────────┐        │
            │             └───────►│  READY  │────────┘
            │                      └─────────┘
            │                          ▲
            │                          │
            │                     再次无任务
            │                     被选中运行
            │                          │
            └──────────────────────────┘
```

### 21.3 空闲任务实现

```c
// 空闲任务 TCB
static tcb_t idle_task = {
    .id = -1,
    .name = "idle",
    .priority = KERN_IDLE_PRIORITY,
    .base_priority = KERN_IDLE_PRIORITY,
    .state = TASK_STATE_READY,
    .time_slice = 0,           // 不参与时间片
    .time_slice_reload = 0,
};

// 空闲任务函数
static void idle_task_func(void *arg) {
    (void)arg;
    uint32_t check_counter = 0;
    
    while (1) {
        // 进入低功耗模式
        __asm volatile("wfi");
        
        // 定期检查栈溢出
        if (++check_counter >= 1000) {
            check_counter = 0;
            check_stack_overflow();
        }
        
        // 喂看门狗
        #if KERN_WATCHDOG_ENABLE
        hal_watchdog_feed();
        #endif
    }
}

// 获取空闲任务
tcb_t *task_get_idle(void) {
    return &idle_task;
}
```

### 21.4 PendSV 中的空闲任务处理

```c
void kern_pendsv_handler(void) {
    tcb_t *current = _current_task;
    
    // 处理当前任务
    if (current) {
        // 空闲任务特殊处理
        if (current->id < 0) {
            // 空闲任务不加入就绪队列，直接设为 READY
            current->state = TASK_STATE_READY;
        } else {
            // 普通任务状态转换
            switch (current->state) {
            case TASK_STATE_RUNNING:
                current->state = TASK_STATE_READY;
                current->time_slice = current->time_slice_reload;
                ready_add(current);
                break;
            // ... 其他状态 ...
            }
        }
    }
    
    // 选择下一个任务
    tcb_t *next = ready_get_highest();
    if (next == NULL) {
        // 无就绪任务，切换到空闲任务
        next = task_get_idle();
    } else {
        ready_remove(next);
    }
    
    next->state = TASK_STATE_RUNNING;
    _next_task = next;
    scheduler.current_task = next;
}
```

---

## 22. 无任务可运行场景

### 22.1 场景分析

| 场景 | 原因 | 处理方式 |
|------|------|----------|
| 启动时无任务 | 未创建/启动任何任务 | 死循环等待或报错 |
| 所有任务阻塞 | 等待资源/延时 | 运行空闲任务 |
| 所有任务挂起 | 被主动挂起 | 运行空闲任务 |
| 所有任务终止 | 任务执行完毕 | 运行空闲任务 |

### 22.2 启动时检查

```c
void sched_start(void) {
    // 检查是否有任务
    int has_task = 0;
    for (int i = 0; i < 4; i++) {
        if (scheduler.ready_bitmap[i] != 0) {
            has_task = 1;
            break;
        }
    }
    
    if (!has_task) {
        // 无任务可运行
        hal_debug_puts("[SCHED] No task to run!\r\n");
        
        // 方案 A: 死循环等待
        while (1) {
            hal_enter_lowpower();
        }
        
        // 方案 B: 触发错误处理
        // kernel_panic("No task to run");
    }
    
    // ... 正常启动流程 ...
}
```

### 22.3 运行时空闲处理

```c
// 当所有任务都阻塞/挂起时，空闲任务自动运行
// 这是正常且预期的行为

void kern_pendsv_handler(void) {
    // ... 处理当前任务 ...
    
    // 选择下一个任务
    tcb_t *next = ready_get_highest();
    
    if (next == NULL) {
        // 无就绪任务，运行空闲任务
        next = task_get_idle();
        
        // 调试输出（可选）
        #if KERN_DEBUG_ENABLE
        hal_debug_puts("[SCHED] Switch to idle\r\n");
        #endif
    } else {
        ready_remove(next);
    }
    
    next->state = TASK_STATE_RUNNING;
    _next_task = next;
}
```

### 22.4 死锁检测（可选）

```c
// 检测是否所有任务都在等待不可能发生的事件
static int check_deadlock(void) {
    int all_blocked = 1;
    
    for (task_id_t id = 0; id < KERN_MAX_TASKS; id++) {
        tcb_t *tcb = &task_pool[id];
        
        if (tcb->state == TASK_STATE_READY ||
            tcb->state == TASK_STATE_RUNNING) {
            all_blocked = 0;
            break;
        }
        
        if (tcb->state == TASK_STATE_BLOCKED &&
            tcb->wake_tick > 0) {
            // 有超时的阻塞，不是死锁
            all_blocked = 0;
            break;
        }
    }
    
    return all_blocked;
}

// 在空闲任务中检测
static void idle_task_func(void *arg) {
    while (1) {
        __asm volatile("wfi");
        
        if (check_deadlock()) {
            hal_debug_puts("[DEADLOCK] All tasks blocked without timeout!\r\n");
            // 可选：触发系统复位
        }
    }
}
```

---

## 23. 错误恢复机制

### 23.1 HardFault 处理

```c
// HardFault 处理函数
void HardFault_Handler(void) {
    tcb_t *current = sched_get_current();
    
    hal_debug_puts("\r\n[HardFault] ");
    
    if (current) {
        hal_debug_puts("Task: ");
        hal_debug_puts(current->name);
        hal_debug_puts(" ID=");
        debug_puthex(current->id);
    }
    
    // 打印异常信息
    uint32_t cfsr = SCB->CFSR;
    uint32_t hfsr = SCB->HFSR;
    uint32_t mmfar = SCB->MMFAR;
    uint32_t bfar = SCB->BFAR;
    
    hal_debug_puts("\r\nCFSR=");
    debug_puthex(cfsr);
    hal_debug_puts(" HFSR=");
    debug_puthex(hfsr);
    hal_debug_puts(" MMFAR=");
    debug_puthex(mmfar);
    hal_debug_puts(" BFAR=");
    debug_puthex(bfar);
    
    // 打印当前栈内容
    uint32_t *sp;
    __asm volatile("mrs %0, psp" : "=r"(sp));
    
    hal_debug_puts("\r\nPSP=");
    debug_puthex((uint32_t)sp);
    hal_debug_puts("\r\nStack: ");
    for (int i = 0; i < 8; i++) {
        debug_puthex(sp[i]);
        hal_debug_putc(' ');
    }
    
    // 恢复策略选择
    #ifdef KERN_FAULT_RECOVERY
    // 方案 A: 终止当前任务，继续运行其他任务
    if (current && current->id >= 0) {
        current->state = TASK_STATE_TERMINATED;
        hal_debug_puts("\r\nTask terminated.\r\n");
        // 触发调度
        hal_trigger_pendsv();
        while (1);  // 等待 PendSV
    }
    #endif
    
    // 方案 B: 系统复位
    hal_debug_puts("\r\nSystem halt.\r\n");
    while (1);
}
```

### 23.2 其他异常处理

```c
// 通用异常处理
#define DEFINE_FAULT_HANDLER(name) \
    void name##_Handler(void) { \
        hal_debug_puts("\r\n[" #name "] Task: "); \
        tcb_t *current = sched_get_current(); \
        if (current) { \
            hal_debug_puts(current->name); \
        } \
        hal_debug_puts("\r\n"); \
        while (1); \
    }

DEFINE_FAULT_HANDLER(NMI)
DEFINE_FAULT_HANDLER(MemManage)
DEFINE_FAULT_HANDLER(BusFault)
DEFINE_FAULT_HANDLER(UsageFault)
```

### 23.3 任务级错误恢复

```c
// 任务看门狗超时处理
typedef struct {
    uint32_t last_feed;
    uint32_t timeout;
    task_id_t task_id;
} task_watchdog_t;

static task_watchdog_t task_wd[KERN_MAX_TASKS];

void task_watchdog_feed(void) {
    tcb_t *current = sched_get_current();
    if (current && current->id >= 0) {
        task_wd[current->id].last_feed = scheduler.tick_count;
    }
}

void task_watchdog_check(void) {
    for (task_id_t id = 0; id < KERN_MAX_TASKS; id++) {
        tcb_t *tcb = &task_pool[id];
        
        if (tcb->state != TASK_STATE_RUNNING) continue;
        
        uint32_t elapsed = scheduler.tick_count - task_wd[id].last_feed;
        if (elapsed > task_wd[id].timeout) {
            hal_debug_puts("[WD] Task ");
            hal_debug_puts(tcb->name);
            hal_debug_puts(" timeout!\r\n");
            
            // 终止任务
            tcb->state = TASK_STATE_TERMINATED;
            hal_trigger_pendsv();
        }
    }
}
```

---

## 24. 调试接口

### 24.1 任务状态打印

```c
/**
 * @brief 打印所有任务状态
 */
void debug_print_tasks(void) {
    hal_debug_puts("\r\n========== Task List ==========\r\n");
    hal_debug_puts("ID  Name       State    Prio  Stack    Ticks\r\n");
    hal_debug_puts("----------------------------------------------\r\n");
    
    // 打印所有任务
    for (task_id_t id = 0; id < KERN_MAX_TASKS; id++) {
        tcb_t *tcb = &task_pool[id];
        
        if (!(task_used_bitmap & (1U << id))) continue;
        
        // ID
        hal_debug_putc('0' + id / 10);
        hal_debug_putc('0' + id % 10);
        hal_debug_putc(' ');
        
        // Name
        hal_debug_puts(tcb->name);
        for (int i = strlen(tcb->name); i < 10; i++) {
            hal_debug_putc(' ');
        }
        
        // State
        const char *state_str[] = {
            "CREATED  ", "READY    ", "RUNNING  ", 
            "BLOCKED  ", "SUSPENDED", "TERMINATED"
        };
        hal_debug_puts(state_str[tcb->state]);
        hal_debug_putc(' ');
        
        // Priority
        debug_puthex(tcb->priority);
        hal_debug_putc(' ');
        
        // Stack usage (approximate)
        uint32_t stack_used = calculate_stack_usage(tcb);
        debug_puthex(stack_used);
        hal_debug_putc(' ');
        
        // Total ticks
        debug_puthex(tcb->total_ticks);
        
        hal_debug_puts("\r\n");
    }
    
    // 打印空闲任务
    hal_debug_puts("-1  idle      ");
    const char *idle_state = (idle_task.state == TASK_STATE_RUNNING) ? 
                             "RUNNING  " : "READY    ";
    hal_debug_puts(idle_state);
    hal_debug_puts("\r\n");
    
    hal_debug_puts("================================\r\n");
}

/**
 * @brief 计算栈使用量
 */
static uint32_t calculate_stack_usage(tcb_t *tcb) {
    uint8_t *stack = (uint8_t *)tcb->stack_base;
    uint32_t unused = 0;
    
    // 从栈底开始检查魔数
    for (uint32_t i = 0; i < tcb->stack_size; i++) {
        if (stack[i] != STACK_MAGIC_BYTE) break;
        unused++;
    }
    
    return tcb->stack_size - unused;
}
```

### 24.2 调度器状态打印

```c
/**
 * @brief 打印调度器状态
 */
void debug_print_scheduler(void) {
    hal_debug_puts("\r\n======== Scheduler Status ========\r\n");
    
    // 当前任务
    tcb_t *current = scheduler.current_task;
    hal_debug_puts("Current: ");
    if (current) {
        hal_debug_puts(current->name);
        hal_debug_puts(" (prio=");
        debug_puthex(current->priority);
        hal_debug_puts(")");
    } else {
        hal_debug_puts("NULL");
    }
    hal_debug_puts("\r\n");
    
    // Tick 计数
    hal_debug_puts("Tick: ");
    debug_puthex(scheduler.tick_count);
    hal_debug_puts("\r\n");
    
    // 就绪队列位图
    hal_debug_puts("Ready Bitmap: ");
    for (int i = 0; i < 4; i++) {
        debug_puthex(scheduler.ready_bitmap[i]);
        hal_debug_putc(' ');
    }
    hal_debug_puts("\r\n");
    
    // 每个优先级的就绪任务数
    hal_debug_puts("Ready Tasks per Priority:\r\n");
    for (int prio = 0; prio < KERN_MAX_PRIORITY; prio++) {
        int count = 0;
        tcb_t *tcb = scheduler.ready_list[prio].head;
        while (tcb) {
            count++;
            tcb = tcb->next;
        }
        if (count > 0) {
            hal_debug_puts("  Prio ");
            debug_puthex(prio);
            hal_debug_puts(": ");
            debug_puthex(count);
            hal_debug_puts(" tasks\r\n");
        }
    }
    
    hal_debug_puts("==================================\r\n");
}
```

### 24.3 调度跟踪

```c
// 调度跟踪缓冲区
#define SCHED_TRACE_SIZE    64

typedef struct {
    uint32_t tick;
    task_id_t from_id;
    task_id_t to_id;
    uint8_t reason;  // 0=yield, 1=block, 2=preempt, 3=wakeup
} sched_trace_t;

static sched_trace_t sched_trace[SCHED_TRACE_SIZE];
static int trace_index = 0;

/**
 * @brief 记录调度事件
 */
void sched_trace_record(task_id_t from, task_id_t to, uint8_t reason) {
    sched_trace_t *t = &sched_trace[trace_index];
    t->tick = scheduler.tick_count;
    t->from_id = from;
    t->to_id = to;
    t->reason = reason;
    
    trace_index = (trace_index + 1) % SCHED_TRACE_SIZE;
}

/**
 * @brief 打印调度跟踪
 */
void debug_print_sched_trace(void) {
    hal_debug_puts("\r\n======== Schedule Trace ========\r\n");
    
    const char *reason_str[] = {"yield", "block", "preempt", "wakeup"};
    
    int idx = trace_index;
    for (int i = 0; i < SCHED_TRACE_SIZE; i++) {
        sched_trace_t *t = &sched_trace[idx];
        if (t->tick == 0) break;
        
        hal_debug_puts("Tick ");
        debug_puthex(t->tick);
        hal_debug_puts(": Task ");
        debug_puthex(t->from_id);
        hal_debug_puts(" -> Task ");
        debug_puthex(t->to_id);
        hal_debug_puts(" (");
        hal_debug_puts(reason_str[t->reason]);
        hal_debug_puts(")\r\n");
        
        idx = (idx + 1) % SCHED_TRACE_SIZE;
    }
    
    hal_debug_puts("================================\r\n");
}
```

### 24.4 调试命令接口

```c
/**
 * @brief 调试命令处理
 * @param cmd 命令字符串
 */
void debug_command(const char *cmd) {
    if (strcmp(cmd, "tasks") == 0) {
        debug_print_tasks();
    }
    else if (strcmp(cmd, "sched") == 0) {
        debug_print_scheduler();
    }
    else if (strcmp(cmd, "trace") == 0) {
        debug_print_sched_trace();
    }
    else if (strcmp(cmd, "stack") == 0) {
        check_stack_overflow();
    }
    else {
        hal_debug_puts("Unknown command: ");
        hal_debug_puts(cmd);
        hal_debug_puts("\r\n");
        hal_debug_puts("Commands: tasks, sched, trace, stack\r\n");
    }
}
```

---

## 25. 完整实现检查清单

### 25.1 必须实现

| 模块 | 功能 | 状态 |
|------|------|------|
| kernel_types.h | TCB、状态枚举、错误码 | ✅ 已有 |
| kernel_config.h | 配置参数、临界区常量 | ✅ 已有 |
| hal.c | BasePri 临界区、栈初始化 | 需更新 |
| context.S | PendSV、SVC 汇编 | 需更新 |
| scheduler.c | 调度器核心逻辑 | 需重写 |
| task.c | 任务管理、栈溢出检测 | 需更新 |

### 25.2 可选实现

| 模块 | 功能 | 优先级 |
|------|------|--------|
| mutex.c | 优先级继承 | 高 |
| 调试接口 | 任务状态打印 | 中 |
| 错误恢复 | HardFault 处理 | 中 |
| 调度跟踪 | 上下文切换记录 | 低 |
| 死锁检测 | 空闲任务中检测 | 低 |

### 25.3 测试用例

| 测试 | 目的 |
|------|------|
| 任务创建/启动 | 验证基本调度 |
| 优先级抢占 | 高优先级任务抢占低优先级 |
| 时间片轮转 | 同优先级任务轮流执行 |
| 任务阻塞/唤醒 | 验证阻塞机制 |
| 任务挂起/恢复 | 验证状态转换 |
| 延时唤醒 | 验证超时处理 |
| 优先级继承 | 验证互斥锁优先级继承 |
| 栈溢出检测 | 验证栈保护机制 |
