# RTOS 调度器使用指南

## 目录

1. [概述](#概述)
2. [核心概念](#核心概念)
3. [调度策略](#调度策略)
4. [API 参考](#api-参考)
5. [使用示例](#使用示例)
6. [常见问题](#常见问题)
7. [调试技巧](#调试技巧)

---

## 概述

### 什么是调度器？

调度器是 RTOS 的核心组件，负责决定哪个任务在什么时候运行。一个好的调度器能够：

- 公平地分配 CPU 时间给各个任务
- 快速响应高优先级任务
- 最小化上下文切换开销

### 本调度器的特点

| 特性 | 说明 |
|------|------|
| **优先级抢占** | 高优先级任务立即抢占低优先级任务 |
| **时间片轮转** | 同优先级任务公平共享 CPU |
| **O(1) 复杂度** | 使用位图快速查找最高优先级任务 |
| **分离式设计** | C 代码负责决策，汇编负责上下文切换 |
| **优先级继承** | 解决优先级反转问题 |

---

## 核心概念

### 任务状态

```
                    ┌─────────────┐
                    │   CREATED   │  任务已创建，未启动
                    └──────┬──────┘
                           │ task_start()
                           ▼
                    ┌─────────────┐
           ┌───────│    READY    │◄───────┐
           │       └──────┬──────┘        │
           │              │ 被调度器选中   │ sched_wakeup()
           │              ▼               │
           │       ┌─────────────┐        │
           │       │   RUNNING   │        │
           │       └──────┬──────┘        │
           │              │               │
           │    ┌─────────┼─────────┐     │
           │    │         │         │     │
           │    ▼         ▼         ▼     │
    sched_add_ready  task_delay   task_suspend
           │    │         │         │     │
           │    │         ▼         ▼     │
           │    │   ┌──────────┐ ┌────────┴───┐
           └────┼───│ BLOCKED  │ │ SUSPENDED  │
                │   └──────────┘ └────────────┘
                │        │
                │        │ 超时或被唤醒
                │        ▼
                └───────►
```

| 状态 | 值 | 说明 |
|------|-----|------|
| `TASK_STATE_CREATED` | 0 | 任务已创建，等待启动 |
| `TASK_STATE_READY` | 1 | 任务就绪，等待运行 |
| `TASK_STATE_RUNNING` | 2 | 任务正在运行 |
| `TASK_STATE_BLOCKED` | 3 | 任务阻塞，等待资源 |
| `TASK_STATE_SUSPENDED` | 4 | 任务挂起 |
| `TASK_STATE_TERMINATED` | 5 | 任务已终止 |

### 优先级

- **数值越小，优先级越高**
- 优先级 0 为最高优先级
- 优先级范围：0 ~ `KERN_MAX_PRIORITY-1`（默认 128 级）

```
优先级:  0    1    2    3  ...  127
         │    │    │    │        │
         ▼    ▼    ▼    ▼        ▼
       最高              ...    最低
```

### 就绪队列

每个优先级维护一个双向链表：

```
优先级 0:  [TaskA] <-> [TaskB] <-> NULL
优先级 1:  [TaskC] <-> NULL
优先级 2:  NULL
...
```

### 位图

使用 4 个 32 位整数表示 128 个优先级：

```
ready_bitmap[0]: 位 0-31   (优先级 0-31)
ready_bitmap[1]: 位 32-63  (优先级 32-63)
ready_bitmap[2]: 位 64-95  (优先级 64-95)
ready_bitmap[3]: 位 96-127 (优先级 96-127)
```

位图某位为 1 表示对应优先级有就绪任务。

---

## 调度策略

### 优先级抢占

当高优先级任务变为就绪时，立即抢占当前运行的低优先级任务：

```
时间 ──────────────────────────────────────►

低优先级任务:  ████████████░░░░░░░░░░░░████████
                          │
高优先级任务:             ████████████
                          │
                     抢占发生点
```

**触发抢占的场景：**

1. 高优先级任务启动（`task_start()`）
2. 高优先级任务被唤醒（`sched_wakeup()`）
3. 中断中使高优先级任务就绪

### 时间片轮转

同优先级任务按时间片轮转执行：

```
时间 ──────────────────────────────────────►

TaskA (优先级 5):  ████████░░░░░░░░░░░░░░░░████████
TaskB (优先级 5):          ████████░░░░░░░░░░░░████
TaskC (优先级 5):                  ████████░░░░░░░░
                          │     │     │
                       时间片  时间片  时间片
```

**时间片配置：**

```c
// kernel_config.h
#define KERN_DEFAULT_TIME_SLICE  10   // 默认时间片（滴答数）
#define KERN_TIME_SLICE          1    // 启用时间片轮转
```

### O(1) 调度

使用位图实现常数时间复杂度的调度：

```c
// 查找最高优先级
static inline int find_highest_prio(void) {
    for (int i = 0; i < 4; i++) {
        if (scheduler.ready_bitmap[i] != 0) {
            // __builtin_ctz: 计算从最低位开始的连续零个数
            return i * 32 + __builtin_ctz(scheduler.ready_bitmap[i]);
        }
    }
    return -1;
}
```

---

## API 参考

### 初始化与启动

#### `sched_init()`

初始化调度器。

```c
void sched_init(void);
```

**必须在创建任何任务之前调用。**

#### `sched_start()`

启动调度器，开始运行第一个任务。

```c
void sched_start(void);
```

**此函数不会返回。**

### 调度控制

#### `sched_yield()`

当前任务主动让出 CPU。

```c
void sched_yield(void);
```

**使用场景：**

```c
void my_task(void *arg) {
    while (1) {
        // 做一些工作
        do_work();

        // 让出 CPU，给其他任务运行机会
        sched_yield();
    }
}
```

#### `sched_add_ready()`

将任务加入就绪队列。

```c
void sched_add_ready(tcb_t *tcb);
```

**通常不直接调用，使用 `task_start()` 代替。**

#### `sched_remove_ready()`

从就绪队列移除任务。

```c
void sched_remove_ready(tcb_t *tcb);
```

### 阻塞与唤醒

#### `sched_block()`

阻塞当前任务。

```c
kern_err_t sched_block(block_reason_t reason, void *obj, uint32_t timeout);
```

| 参数 | 说明 |
|------|------|
| `reason` | 阻塞原因（SLEEP、MUTEX、SEM 等） |
| `obj` | 阻塞对象指针 |
| `timeout` | 超时时间（滴答数），0 表示无限等待 |

**返回值：**

| 值 | 说明 |
|-----|------|
| `KERN_OK` | 正常唤醒 |
| `KERN_ERR_TIMEOUT` | 超时唤醒 |

#### `sched_wakeup()`

唤醒阻塞的任务。

```c
void sched_wakeup(tcb_t *tcb, kern_err_t result);
```

### 状态查询

#### `sched_get_current()`

获取当前运行任务的 TCB。

```c
tcb_t *sched_get_current(void);
```

#### `sched_get_highest_ready()`

获取最高优先级的就绪任务。

```c
tcb_t *sched_get_highest_ready(void);
```

#### `sched_get_tick_count()`

获取系统滴答计数。

```c
uint32_t sched_get_tick_count(void);
```

---

## 使用示例

### 示例 1: 基本任务调度

```c
#include "kernel.h"
#include "task.h"

// 高优先级任务
void high_task(void *arg) {
    while (1) {
        // 高优先级任务会抢占低优先级任务
        do_critical_work();
        task_delay(100);  // 阻塞 100 滴答
    }
}

// 低优先级任务
void low_task(void *arg) {
    while (1) {
        do_background_work();
        task_delay(1000);
    }
}

int main(void) {
    kern_init();

    task_id_t high = task_create("high", high_task, NULL, 2, 0);  // 优先级 2
    task_id_t low = task_create("low", low_task, NULL, 10, 0);    // 优先级 10

    task_start(high);
    task_start(low);

    kern_start();  // 启动调度器，不会返回

    return 0;
}
```

### 示例 2: 任务协作

```c
void producer_task(void *arg) {
    while (1) {
        produce_data();
        sem_post(&sem);  // 发送信号
        task_yield();    // 让出 CPU
    }
}

void consumer_task(void *arg) {
    while (1) {
        sem_wait(&sem);  // 等待信号
        consume_data();
    }
}
```

### 示例 3: 优先级继承

```c
mutex_id_t mutex;

void high_task(void *arg) {
    task_delay(10);  // 让低优先级任务先获取锁

    mutex_lock(mutex, 0);  // 等待锁
    // 此时低优先级任务的优先级会被提升到 2
    access_shared_resource();
    mutex_unlock(mutex);
}

void low_task(void *arg) {
    mutex_lock(mutex, 0);  // 获取锁
    // 优先级可能被提升到 2
    do_work_with_lock();
    mutex_unlock(mutex);  // 释放锁，优先级恢复
}

int main(void) {
    kern_init();
    mutex = mutex_create();

    task_id_t high = task_create("high", high_task, NULL, 2, 0);
    task_id_t low = task_create("low", low_task, NULL, 20, 0);

    task_start(low);
    task_start(high);

    kern_start();
}
```

### 示例 4: 任务退出

```c
void worker_task(void *arg) {
    int count = 0;

    while (count < 10) {
        do_work();
        count++;
        task_delay(100);
    }

    // 任务完成后自动退出
    // 不需要显式调用 task_exit()
}

int main(void) {
    kern_init();

    task_id_t worker = task_create("worker", worker_task, NULL, 5, 0);
    task_start(worker);

    kern_start();
}
```

---

## 常见问题

### Q1: 为什么任务不运行？

**检查清单：**

1. 是否调用了 `task_start()`？
2. 任务优先级是否正确？
3. 是否有更高优先级任务一直运行？

```c
// 错误：忘记启动任务
task_id_t task = task_create("my_task", my_func, NULL, 5, 0);
// 缺少 task_start(task);

// 正确
task_id_t task = task_create("my_task", my_func, NULL, 5, 0);
task_start(task);
```

### Q2: 为什么高优先级任务被阻塞？

**可能原因：**

1. 任务在等待互斥锁或信号量
2. 任务调用了 `task_delay()`
3. 任务调用了阻塞 API

### Q3: 如何避免优先级反转？

使用带有优先级继承的互斥锁：

```c
// kernel_config.h
#define KERN_MUTEX_PI  1  // 启用优先级继承
```

### Q4: 时间片如何配置？

```c
// kernel_config.h
#define KERN_TIME_SLICE           1    // 启用时间片轮转
#define KERN_DEFAULT_TIME_SLICE   10   // 默认时间片（滴答数）
```

### Q5: 如何调试调度问题？

启用调试输出：

```c
// 在 kern_pendsv_handler() 中添加
hal_debug_puts("[SCHED] current=");
hal_debug_puts(current->name);
hal_debug_puts(" next=");
hal_debug_puts(next->name);
hal_debug_puts("\r\n");
```

---

## 调试技巧

### 1. 跟踪上下文切换

```c
void kern_pendsv_handler(void) {
    // ... 现有代码 ...

    // 打印切换信息
    if (current && next) {
        hal_debug_puts("[SWITCH] ");
        hal_debug_puts(current->name);
        hal_debug_puts(" -> ");
        hal_debug_puts(next->name);
        hal_debug_puts("\r\n");
    }
}
```

### 2. 检查就绪队列

```c
void debug_print_ready_queue(void) {
    for (int prio = 0; prio < KERN_MAX_PRIORITY; prio++) {
        tcb_t *tcb = scheduler.ready_list[prio].head;
        if (tcb) {
            hal_debug_puts("Priority ");
            // 打印优先级
            hal_debug_puts(": ");
            while (tcb) {
                hal_debug_puts(tcb->name);
                hal_debug_puts(" -> ");
                tcb = tcb->next;
            }
            hal_debug_puts("\r\n");
        }
    }
}
```

### 3. 统计 CPU 使用率

```c
#if KERN_TASK_STATS
void print_cpu_usage(void) {
    task_id_t id = -1;
    while ((id = task_get_next(id)) != KERN_INVALID_ID) {
        tcb_t *tcb = task_get_tcb(id);
        hal_debug_puts(tcb->name);
        hal_debug_puts(": ");
        // 打印 CPU 使用率
        hal_debug_puts("\r\n");
    }
}
#endif
```

### 4. HardFault 分析

如果遇到 HardFault，检查：

1. PSP 值是否有效（`PSP: 0x00000020` 表示无效）
2. CFSR 寄存器指示的错误类型
3. PC 值指示的出错位置

---

## 参考资料

- [../design/SCHEDULER_DESIGN.md](./../design/SCHEDULER_DESIGN.md) - 调度器设计文档
- [SCHEDULER_DEBUG_NOTES.md](./SCHEDULER_DEBUG_NOTES.md) - 调试问题与解决方案
- [kernel_types.h](../src/kernel/include/kernel_types.h) - 类型定义
- [scheduler.h](../src/kernel/core/scheduler.h) - API 声明
P1 微内核核心能力
  5. IPC：Endpoint / Channel
  文件：src/kernel/ipc/endpoint.c、src/kernel/ipc/channel.c
  问题：已有雏形，但还不像真正微内核 IPC。
  需要补：同步调用语义、server reply 绑定、超时取消、死亡通知、权限检查、消息
  copy 或共享内存边界检查。

  6. Capability 能力系统
     文件：src/kernel/cap/capability.c
     问题：现在更像 ID 权限表，和内核对象生命周期绑定还不够。
     需要补：对象引用计数、cap revoke 级联、per-task CSpace、cap 类型检查、IPC
     传递规则。
  7. VFS / 文件描述符
     文件：src/kernel/vfs/vfs.c、src/kernel/vfs/ramfs.c、src/kernel/vfs/devfs.c
     问题：shell 里 ls: readdir not supported 已经说明目录遍历不完整。
     需要补：readdir、路径规范化、mount、权限、fd 生命周期、设备文件统一 ioctl/
     read/write 语义。

  P2 服务化和工程化
  8. Timer / IRQ / BH
  文件：src/kernel/timer/timer.c、src/kernel/irq/irq.c、src/kernel/irq/bh.c
  问题：timer service、bottom-half service 已有，但有不少 while(1) 服务循环和部
  分 stub。
  需要补：服务任务栈预算、队列满处理、删除时唤醒等待者、IRQ 线程化策略、BH 删除/
  取消完善。

  9. 驱动模型
     文件：src/kernel/dev/device.c、src/drivers/uart_dev.c
     问题：目前设备模型偏简单，UART 能用，但没有完整驱动框架。
     需要补：统一 probe/remove、设备节点和 devfs 绑定、阻塞 read/write、select/
     poll 或事件通知、驱动权限。
  10. 内存管理
     文件：src/kernel/mem/mem.c、src/kernel/mem/mempool.c
     问题：可用但偏静态/简单。
     需要补：用户态分配边界、对象 slab、泄漏统计、OOM 策略、cap 与内存对象绑定。
  11. Shell / 诊断工具
     文件：src/app/shell.c
     页或限速。
  12. Trace / Stats
     文件：src/kernel/trace/trace.c、src/kernel/stats/stats.c
     问题：接口有，但作为定位随机问题还不够。
  总体判断：
  你现在最该优先优化的是 任务生命周期 + syscall/SVC + MPU 隔离 + IPC/
  capability。这四块决定它是不是“微内核”。VFS、shell、驱动可以后置，它们现在更多
  是验证和演示层。
