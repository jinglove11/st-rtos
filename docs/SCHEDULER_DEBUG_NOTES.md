# 调度器问题分析与解决方案

## 概述

在实现 RTOS 调度器过程中遇到了多个关键问题，本文档记录了这些问题及其解决方案。

---

## 问题 1: 任务退出后系统挂起

### 现象

当任务调用 `task_exit()` 后，系统挂起在 `while(1)` 死循环中，无法切换到下一个任务。

### 根本原因

PendSV 处理程序在保存上下文时，没有检查任务状态。当 TERMINATED 状态的任务触发 PendSV 时：

1. PendSV 保存当前任务上下文（包括返回地址到 `while(1)`）
2. `kern_pendsv_handler()` 处理 TERMINATED 状态并选择下一个任务
3. PendSV 恢复下一个任务的上下文
4. **但异常返回时使用的是保存的上下文**，导致返回到 `while(1)` 而不是新任务

### 解决方案

在 PendSV 汇编代码中添加状态检查，跳过 TERMINATED 任务的上下文保存：

```asm
// context.S
PendSV_Handler:
    cpsid   i
    ldr     r0, =_current_task
    ldr     r0, [r0]

    cmp     r0, #0
    beq     .L_select_next

    // 检查任务状态是否为 TERMINATED (5)
    // TCB 结构: sp(4) + name[16](16) + id(4) + priority(1) + base_priority(1) + state(1) = offset 26
    ldrb    r1, [r0, #26]
    cmp     r1, #5
    beq     .L_select_next  // TERMINATED 任务不保存上下文

    // 保存当前任务上下文
    mrs     r1, psp
    stmdb   r1!, {r4-r11}
    str     r1, [r0]
```

**关键点**: TCB 中 state 字段的偏移量计算：
- `sp`: 4 字节 (指针)
- `name[16]`: 16 字节
- `id`: 4 字节
- `priority`: 1 字节
- `base_priority`: 1 字节
- `state`: 1 字节 → 偏移 26

---

## 问题 2: 已回收任务仍被调度器选中

### 现象

当任务退出并回收后，调度器仍然选中该任务（显示空名称），导致 HardFault：

```
[PENDSV] highest_ready=
[PENDSV] next=

!!! HARDFAULT !!!
PSP: 0x00000020
```

### 根本原因

当任务 yield 时被加入就绪队列，退出时 TCB 被清零，但：

1. 任务仍在就绪队列链表中
2. 位图中对应位仍然置位
3. `find_highest_prio()` 找到置位的位
4. `ready_list_get_head()` 返回已清零的 TCB

### 解决方案

#### 方案 A: 在回收前从就绪队列移除

```c
// scheduler.c - kern_pendsv_handler()
case TASK_STATE_TERMINATED:
    {
        uint8_t prio = current->priority;
        ready_list_t *list = &scheduler.ready_list[prio];

        // 检查是否在就绪队列中
        if (list->head == current || current->next != NULL || current->prev != NULL) {
            ready_list_remove_internal(current);
        }

        extern void task_reclaim(tcb_t *tcb);
        task_reclaim(current);
    }
    break;
```

#### 方案 B: 在选择时跳过无效 TCB

```c
// scheduler.c - sched_get_highest_ready()
tcb_t *sched_get_highest_ready(void) {
    int highest_prio = find_highest_prio();
    if (highest_prio < 0) {
        return NULL;
    }

    tcb_t *tcb = ready_list_get_head((uint8_t)highest_prio);

    // 跳过无效的 TCB (已被回收的任务)
    while (tcb && tcb->name[0] == '\0') {
        // 清除位图并清空就绪队列
        bitmap_clear((uint8_t)highest_prio);
        scheduler.ready_list[highest_prio].head = NULL;
        scheduler.ready_list[highest_prio].tail = NULL;

        // 重新查找
        highest_prio = find_highest_prio();
        if (highest_prio < 0) {
            return NULL;
        }
        tcb = ready_list_get_head((uint8_t)highest_prio);
    }

    return tcb;
}
```

#### 方案 C: 选择时检查空名称

```c
// scheduler.c - kern_pendsv_handler()
if (next == NULL || next->name[0] == '\0') {
    // 没有就绪任务，切换到 idle
    next = task_get_idle();
} else {
    ready_list_remove_internal(next);
}
```

**最佳实践**: 三个方案结合使用，提供多层保护。

---

## 问题 3: 空闲任务未被正确初始化

### 现象

当所有任务阻塞时，调度器找不到可运行的任务。

### 根本原因

空闲任务初始化时状态设为 `TASK_STATE_CREATED`，而不是 `TASK_STATE_READY`。

### 解决方案

```c
// task.c - task_init()
idle_task.state = TASK_STATE_READY;  // 空闲任务始终就绪
```

---

## 问题 4: 优先级继承使用错误任务

### 现象

优先级继承测试失败，高优先级任务无法获取锁。

### 根本原因

`mutex_priority_inherit()` 使用 `sched_get_current()` 获取当前任务，但应该获取锁的持有者：

```c
// 错误
tcb_t *owner = sched_get_current();

// 正确
tcb_t *owner = task_get_tcb(mutex->owner);
```

### 解决方案

```c
// mutex.c
static void mutex_priority_inherit(mutex_t *mutex, tcb_t *waiter) {
    if (mutex->owner < 0) return;

    extern tcb_t *task_get_tcb(task_id_t task_id);
    tcb_t *owner = task_get_tcb(mutex->owner);  // 获取锁持有者
    if (owner == NULL) return;

    if (waiter->priority < owner->priority) {
        owner->priority = waiter->priority;

        if (owner->state == TASK_STATE_READY) {
            extern void sched_reinsert_by_priority(tcb_t *tcb);
            sched_reinsert_by_priority(owner);
        }
    }
}
```

---

## 问题 5: 任务回收后 TCB 未清零

### 现象

已回收任务的 TCB 仍有残留数据，导致调度器错误选中。

### 解决方案

```c
// task.c - task_reclaim()
void task_reclaim(tcb_t *tcb) {
    if (tcb == NULL || tcb->id < 0) return;

    task_id_t id = tcb->id;
    free_task_id(id);

    // 必须清零 TCB
    memset(tcb, 0, sizeof(tcb_t));
}
```

---

## 调试技巧

### 1. 添加 PendSV 跟踪

```c
hal_debug_puts("[PENDSV] current=");
hal_debug_puts(current->name);
hal_debug_puts(" state=");
// 打印状态名称
```

### 2. 检查就绪队列

```c
hal_debug_puts("[SCHED] highest_ready=");
if (next) {
    hal_debug_puts(next->name);
} else {
    hal_debug_puts("NULL");
}
```

### 3. HardFault 分析

检查 PSP 值：
- `PSP: 0x00000020` → 栈指针无效，通常是访问了空指针或已清零的 TCB

---

## 测试验证

所有 50 个测试通过：

```
========================================
         TEST SUMMARY
========================================
Passed: 50
Failed: 0
========================================
All tests PASSED!
```

关键测试用例：
- Test 12: Priority Inheritance - 验证优先级继承
- Test 13: Task Yield - 验证任务让出 CPU
- Test 14: Idle Task - 验证空闲任务
- Test 15: Stress Test - 验证多任务压力测试
- Test 16: Self Delete Protection - 验证任务删除保护

---

## 文件修改清单

| 文件 | 修改内容 |
|------|----------|
| `src/arch/arm/cortex-m7/context.S` | 添加 TERMINATED 状态检查 |
| `src/kernel/core/scheduler.c` | 修复任务回收、无效 TCB 检测 |
| `src/kernel/task/task.c` | 修复空闲任务初始化、TCB 清零 |
| `src/kernel/ipc/mutex.c` | 修复优先级继承 |

---

## 参考资料

- ARM Cortex-M7 技术参考手册
- SCHEDULER_DESIGN.md - 调度器设计文档
- kernel_types.h - 任务状态定义
