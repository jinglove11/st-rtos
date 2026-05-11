# Scheduler Rewrite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rewrite the RTOS scheduler with proper separation of concerns - scheduler core only modifies state/queues, PendSV handles all context switching.

**Architecture:** Priority preemptive + time slice round-robin scheduling. BasePri-based critical sections preserve high-priority hardware interrupt response. PendSV unified context switching eliminates the stack corruption bug where both C function prologue and PendSV saved registers to the same location.

**Tech Stack:** C, ARM Cortex-M7 assembly, STM32F767ZI

---

## File Structure

| File | Responsibility | Action |
|------|---------------|--------|
| `src/kernel/include/kernel_config.h` | Critical section constants, stack magic | Modify |
| `src/hal/hal.h` | HAL interface declarations | Modify |
| `src/arch/arm/cortex-m7/hal.c` | BasePri critical sections, stack init with magic | Modify |
| `src/arch/arm/cortex-m7/context.S` | PendSV/SVC handlers | Modify |
| `src/kernel/core/scheduler.c` | Scheduler core logic (rewrite) | Rewrite |
| `src/kernel/core/scheduler.h` | Scheduler interface | Modify |
| `src/kernel/task/task.c` | Task management, stack overflow check | Modify |
| `src/app/main.c` | Test cases | Modify |

---

## Task 1: Update HAL Interface

**Files:**
- Modify: `src/hal/hal.h`

- [ ] **Step 1: Add BasePri-based critical section interface**

```c
#ifndef HAL_H
#define HAL_H

#include <stdint.h>

void hal_cpu_init(void);
void hal_debug_putc(char c);
void hal_debug_puts(const char *s);

void hal_systick_init(uint32_t rate_hz);
uint32_t hal_systick_get(void);
void hal_systick_enable(void);
void hal_systick_disable(void);

void hal_irq_enable(void);
void hal_irq_disable(void);
uint32_t hal_irq_save(void);
void hal_irq_restore(uint32_t primask);

// BasePri-based critical section (preserves high-priority interrupts)
uint32_t hal_enter_critical(void);
void hal_exit_critical(uint32_t basepri);

// Interrupt priority initialization
void hal_interrupt_priority_init(void);

void hal_irq_set_priority(uint32_t irq, uint32_t priority);
void hal_irq_enable_irq(uint32_t irq);
void hal_irq_disable_irq(uint32_t irq);
void hal_irq_clear_pending(uint32_t irq);

void hal_enter_lowpower(void);
void hal_exit_lowpower(void);

void *hal_stack_init(void *stack_top, uint32_t stack_size, void *entry, void *arg, void *exit);

void hal_trigger_pendsv(void);
void hal_trigger_svc(uint32_t svc_num);
void hal_trigger_first_switch(void);

void hal_watchdog_init(void);
void hal_watchdog_feed(void);

uint32_t hal_get_tick_count(void);
void hal_set_tick_count(uint32_t count);

#endif
```

- [ ] **Step 2: Commit**

```bash
git add src/hal/hal.h
git commit -m "feat(hal): add BasePri critical section interface"
```

---

## Task 2: Update HAL Implementation

**Files:**
- Modify: `src/arch/arm/cortex-m7/hal.c`

- [ ] **Step 1: Add BasePri critical section implementation**

Add after the existing `hal_irq_restore` function:

```c
/*============================================================================
 * BasePri 临界区实现
 *============================================================================*/

uint32_t hal_enter_critical(void) {
    uint32_t basepri;
    __asm volatile("mrs %0, basepri" : "=r"(basepri));
    __asm volatile("msr basepri, %0" :: "r"(SCHED_CRITICAL_PRIORITY << 4));
    __asm volatile("isb");
    return basepri;
}

void hal_exit_critical(uint32_t basepri) {
    __asm volatile("msr basepri, %0" :: "r"(basepri));
}

void hal_interrupt_priority_init(void) {
    // 设置 SysTick 优先级为最低
    SCB->SHP[11] = SYSTICK_PRIORITY << 4;  // SysTick
    
    // 设置 PendSV 优先级为最低
    SCB->SHP[14] = PENDSV_PRIORITY << 4;   // PendSV
    
    // 设置 SVC 优先级为最高（用于首次切换）
    SCB->SHP[7] = 0;                        // SVC
}
```

- [ ] **Step 2: Update hal_stack_init to fill magic bytes**

Replace the existing `hal_stack_init` function:

```c
void *hal_stack_init(void    *stack_top,
                     uint32_t stack_size,
                     void    *entry,
                     void    *arg,
                     void    *exit)
{
    (void)stack_size;

    uint8_t *stack_base = (uint8_t *)stack_top - stack_size;
    
    // 填充魔数用于栈溢出检测
    for (int i = 0; i < 16; i++) {
        stack_base[i] = STACK_MAGIC_BYTE;
    }

    uint32_t *sp = (uint32_t *)((uint8_t *)stack_top);

    // Cortex-M 硬件异常栈帧格式 (从高地址到低地址):
    // xPSR, PC, LR, R12, R3, R2, R1, R0
    // PendSV 软件帧: R4-R11 (在硬件帧之前)

    // 1. 初始化硬件帧 (异常返回时硬件自动恢复)
    sp--; *sp = 0x01000000UL;   // xPSR (Thumb bit set)
    sp--; *sp = (uint32_t)entry; // PC (任务入口)
    sp--; *sp = (uint32_t)exit;  // LR (任务退出处理)
    sp--; *sp = 0;               // R12
    sp--; *sp = 0;               // R3
    sp--; *sp = 0;               // R2
    sp--; *sp = 0;               // R1
    sp--; *sp = (uint32_t)arg;   // R0 (任务参数)

    // 2. 初始化软件帧 (PendSV 恢复)
    sp--; *sp = 0;  // R11
    sp--; *sp = 0;  // R10
    sp--; *sp = 0;  // R9
    sp--; *sp = 0;  // R8
    sp--; *sp = 0;  // R7
    sp--; *sp = 0;  // R6
    sp--; *sp = 0;  // R5
    sp--; *sp = 0;  // R4

    // 3. 确保 8 字节对齐 (AAPCS 要求)
    if ((uint32_t)sp & 4) {
        sp--;
        *sp = 0;
    }

    return sp;
}
```

- [ ] **Step 3: Add tick count getter/setter**

Add after `hal_systick_disable`:

```c
static volatile uint32_t systick_count = 0;

uint32_t hal_get_tick_count(void) {
    return systick_count;
}

void hal_set_tick_count(uint32_t count) {
    systick_count = count;
}
```

- [ ] **Step 4: Update SysTick_Handler**

Update the SysTick_Handler to use systick_count:

```c
void SysTick_Handler(void) {
    systick_count++;

    // 调用内核滴答处理
    extern void kern_tick_handler(void);
    kern_tick_handler();
}
```

- [ ] **Step 5: Update hal_cpu_init to call interrupt priority init**

Add call to `hal_interrupt_priority_init()` in `hal_cpu_init`:

```c
void hal_cpu_init(void) {
    // 设置向量表位置
    SCB->VTOR = 0x08000000UL;

    // 初始化中断优先级
    hal_interrupt_priority_init();
}
```

- [ ] **Step 6: Commit**

```bash
git add src/arch/arm/cortex-m7/hal.c
git commit -m "feat(hal): implement BasePri critical sections and stack magic bytes"
```

---

## Task 3: Rewrite PendSV Handler

**Files:**
- Modify: `src/arch/arm/cortex-m7/context.S`

- [ ] **Step 1: Rewrite context.S with proper PendSV handling**

```asm
/**
 * @file context.S
 * @brief Cortex-M7 上下文切换
 */

    .syntax unified
    .cpu cortex-m7
    .thumb

/*============================================================================
 * 外部符号
 *============================================================================*/

    .extern _current_task
    .extern _next_task
    .extern kern_pendsv_handler

/*============================================================================
 * PendSV 异常处理
 *
 * 关键设计：
 * - 调度器核心（C代码）只修改状态和队列
 * - PendSV（汇编）负责保存/恢复寄存器
 * - 这消除了 C 函数调用栈与 PendSV 保存位置冲突的 bug
 *============================================================================*/

    .section .text
    .global PendSV_Handler
    .type PendSV_Handler, %function
    .align 2

PendSV_Handler:
    // 禁用中断 (保护调度过程)
    cpsid   i

    // 获取当前任务 TCB
    ldr     r0, =_current_task
    ldr     r0, [r0]

    // 如果当前任务为 NULL，跳过保存 (首次切换)
    cmp     r0, #0
    beq     .L_select_next

    // 保存当前任务上下文
    mrs     r1, psp             // 获取进程栈指针
    stmdb   r1!, {r4-r11}       // 保存 R4-R11
    str     r1, [r0]            // 保存 SP 到 TCB->sp

.L_select_next:
    // 调用 C 函数选择下一任务
    // kern_pendsv_handler 会设置 _next_task
    push    {lr}
    bl      kern_pendsv_handler
    pop     {lr}

    // 获取下一任务的栈指针
    ldr     r0, =_next_task
    ldr     r0, [r0]
    cmp     r0, #0
    beq     .L_pendsv_halt
    ldr     r1, [r0]            // r1 = SP

    // 恢复上下文
    ldmia   r1!, {r4-r11}       // 恢复 R4-R11
    msr     psp, r1             // 设置 PSP

    // 使能中断
    cpsie   i

    // 从异常返回
    bx      lr

.L_pendsv_halt:
    // _next_task 为 NULL，死循环
    b       .

    .size PendSV_Handler, . - PendSV_Handler

/*============================================================================
 * SVC 处理 - 用于启动第一个任务
 *
 * 首次切换时 _current_task 为 NULL
 * 直接从 _next_task 恢复上下文并开始执行
 *============================================================================*/

    .section .text
    .global SVC_Handler
    .type SVC_Handler, %function
    .align 2

SVC_Handler:
    // 获取下一任务
    ldr     r0, =_next_task
    ldr     r0, [r0]
    ldr     r1, [r0]            // r1 = SP

    // 恢复 R4-R11
    ldmia   r1!, {r4-r11}

    // 设置 PSP
    msr     psp, r1

    // 设置 CONTROL 寄存器使用 PSP (SPSEL = 1)
    movs    r2, #2
    msr     control, r2
    isb                             // 指令同步屏障

    // 更新 _current_task = _next_task
    ldr     r2, =_current_task
    ldr     r3, =_next_task
    ldr     r3, [r3]
    str     r3, [r2]

    // 使能中断
    cpsie   i

    // 从异常返回到线程模式 (使用 PSP)
    ldr     lr, =0xFFFFFFFD
    bx      lr

    .size SVC_Handler, . - SVC_Handler

/*============================================================================
 * 触发第一次调度 (通过 SVC 0)
 *============================================================================*/

    .section .text
    .global hal_trigger_first_switch
    .type hal_trigger_first_switch, %function
    .align 2

hal_trigger_first_switch:
    // SVC 优先级已在 hal_interrupt_priority_init 中设置
    // 触发 SVC
    svc     #0

    // 不应该返回
    b       .

    .size hal_trigger_first_switch, . - hal_trigger_first_switch

    .end
```

- [ ] **Step 2: Commit**

```bash
git add src/arch/arm/cortex-m7/context.S
git commit -m "feat(context): rewrite PendSV with proper context handling"
```

---

## Task 4: Rewrite Scheduler Core

**Files:**
- Rewrite: `src/kernel/core/scheduler.c`

- [ ] **Step 1: Rewrite scheduler.c with separated design**

```c
/**
 * @file scheduler.c
 * @brief 调度器实现 - 分离式设计
 *
 * 核心原则：调度器核心（C 代码）只负责"决策"
 * PendSV（汇编）负责"执行"（保存/恢复寄存器）
 */

#include "scheduler.h"
#include "kernel_config.h"
#include "kernel_types.h"
#include "task.h"
#include "hal.h"
#include <string.h>

extern tcb_t *task_get_tcb(task_id_t task_id);
extern task_id_t task_get_next(task_id_t task_id);
extern uint32_t task_get_used_bitmap(void);

/*============================================================================
 * 调试辅助函数
 *============================================================================*/

static void debug_puthex(uint32_t v) {
    const char hex[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) {
        hal_debug_putc(hex[(v >> i) & 0xF]);
    }
}

/*============================================================================
 * PendSV 处理 (汇编调用)
 *============================================================================*/

// 当前任务和下一任务指针 (汇编访问)
tcb_t *volatile _current_task = NULL;
tcb_t *volatile _next_task = NULL;

/*============================================================================
 * 内部数据结构
 *============================================================================*/

// 就绪队列 (每个优先级一个链表)
typedef struct {
    tcb_t *head;
    tcb_t *tail;
} ready_list_t;

// 调度器数据
static struct {
    tcb_t *current_task;
    ready_list_t ready_list[KERN_MAX_PRIORITY];
    volatile uint32_t ready_bitmap[4];
    volatile uint32_t tick_count;
    volatile int need_resched;
    int started;

#if KERN_TASK_STATS
    uint32_t last_stat_tick;
#endif

} scheduler;

/*============================================================================
 * 位图操作 (快速查找最高优先级)
 *============================================================================*/

static inline int find_highest_prio(void) {
    for (int i = 0; i < 4; i++) {
        if (scheduler.ready_bitmap[i] != 0) {
            return i * 32 + __builtin_ctz(scheduler.ready_bitmap[i]);
        }
    }
    return -1;
}

static inline void bitmap_set(uint8_t prio) {
    scheduler.ready_bitmap[prio / 32] |= (1U << (prio % 32));
}

static inline void bitmap_clear(uint8_t prio) {
    scheduler.ready_bitmap[prio / 32] &= ~(1U << (prio % 32));
}

/*============================================================================
 * 就绪队列操作 (内部函数，调用者负责临界区)
 *============================================================================*/

static void ready_list_add_internal(tcb_t *tcb) {
    uint8_t prio = tcb->priority;
    ready_list_t *list = &scheduler.ready_list[prio];

    tcb->next = NULL;
    tcb->prev = list->tail;

    if (list->tail) {
        list->tail->next = tcb;
    } else {
        list->head = tcb;
    }
    list->tail = tcb;

    bitmap_set(prio);
}

static void ready_list_remove_internal(tcb_t *tcb) {
    uint8_t prio = tcb->priority;
    ready_list_t *list = &scheduler.ready_list[prio];

    if (tcb->prev) {
        tcb->prev->next = tcb->next;
    } else {
        list->head = tcb->next;
    }

    if (tcb->next) {
        tcb->next->prev = tcb->prev;
    } else {
        list->tail = tcb->prev;
    }

    tcb->next = NULL;
    tcb->prev = NULL;

    if (list->head == NULL) {
        bitmap_clear(prio);
    }
}

// 获取就绪队列头
static tcb_t *ready_list_get_head(uint8_t prio) {
    return scheduler.ready_list[prio].head;
}

/*============================================================================
 * 公开接口实现
 *============================================================================*/

void sched_init(void) {
    uint32_t crit = hal_enter_critical();

    for (int i = 0; i < KERN_MAX_PRIORITY; i++) {
        scheduler.ready_list[i].head = NULL;
        scheduler.ready_list[i].tail = NULL;
    }
    scheduler.current_task = NULL;
    for (int i = 0; i < 4; i++) {
        scheduler.ready_bitmap[i] = 0;
    }
    scheduler.tick_count = 0;
    scheduler.need_resched = 0;
    scheduler.started = 0;

    hal_exit_critical(crit);
}

void sched_start(void) {
    uint32_t crit = hal_enter_critical();

    // 检查是否有任务
    int has_task = 0;
    for (int i = 0; i < 4; i++) {
        if (scheduler.ready_bitmap[i] != 0) {
            has_task = 1;
            break;
        }
    }
    
    if (!has_task) {
        hal_exit_critical(crit);
        hal_debug_puts("[SCHED] No task to run!\r\n");
        while (1) {
            hal_enter_lowpower();
        }
    }

    tcb_t *first = sched_get_highest_ready();
    if (first == NULL) {
        hal_exit_critical(crit);
        hal_debug_puts("[SCHED] get_highest_ready returned NULL!\r\n");
        while (1) {
            hal_enter_lowpower();
        }
    }

    ready_list_remove_internal(first);

    first->state = TASK_STATE_RUNNING;

    _current_task = NULL;  // 关键：首次切换标记
    _next_task = first;
    scheduler.current_task = first;

    scheduler.started = 1;

    hal_exit_critical(crit);

    // 启动 SysTick
    hal_systick_init(KERN_TICK_RATE_HZ);

    // 使能全局中断
    hal_irq_enable();

    // 触发首次切换 (SVC)
    hal_trigger_first_switch();

    // 不应该到达这里
    while (1);
}

void sched_yield(void) {
    scheduler.need_resched = 1;
    hal_trigger_pendsv();
}

void sched_add_ready(tcb_t *tcb) {
    uint32_t crit = hal_enter_critical();

    if (tcb->state == TASK_STATE_READY) {
        hal_exit_critical(crit);
        return;  // 已经在就绪队列
    }

    tcb->state = TASK_STATE_READY;
    ready_list_add_internal(tcb);

    // 如果调度器已启动且优先级高于当前任务, 触发调度
    if (scheduler.started &&
        scheduler.current_task &&
        tcb->priority < scheduler.current_task->priority) {
        scheduler.need_resched = 1;
        hal_trigger_pendsv();
    }

    hal_exit_critical(crit);
}

void sched_remove_ready(tcb_t *tcb) {
    uint32_t crit = hal_enter_critical();

    if (tcb->state != TASK_STATE_READY) {
        hal_exit_critical(crit);
        return;
    }

    ready_list_remove_internal(tcb);
    hal_exit_critical(crit);
}

kern_err_t sched_block(block_reason_t reason, void *obj, uint32_t timeout) {
    tcb_t *current = scheduler.current_task;

    if (current == NULL) {
        return KERN_ERR_STATE;
    }

    uint32_t crit = hal_enter_critical();

    // 从就绪队列移除当前任务
    if (current->state == TASK_STATE_READY) {
        ready_list_remove_internal(current);
    }

    current->state = TASK_STATE_BLOCKED;
    current->block_reason = reason;
    current->block_obj = obj;
    current->block_result = KERN_OK;

    if (timeout > 0) {
        current->wake_tick = scheduler.tick_count + timeout;
    } else {
        current->wake_tick = 0;
    }

    scheduler.need_resched = 1;

    // 解锁并触发 PendSV
    hal_exit_critical(crit);
    hal_trigger_pendsv();

    // 当任务被唤醒时，会从这里继续执行
    // 此时 block_result 已经被 sched_wakeup 设置
    return current->block_result;
}

void sched_wakeup(tcb_t *tcb, kern_err_t result) {
    uint32_t crit = hal_enter_critical();

    if (tcb->state != TASK_STATE_BLOCKED) {
        hal_exit_critical(crit);
        return;
    }

    tcb->block_reason = BLOCK_REASON_NONE;
    tcb->block_obj = NULL;
    tcb->block_result = result;
    tcb->wake_tick = 0;

    tcb->state = TASK_STATE_READY;
    ready_list_add_internal(tcb);

    // 检查是否需要抢占
    tcb_t *current = scheduler.current_task;
    if (current && tcb->priority < current->priority) {
        scheduler.need_resched = 1;
        hal_trigger_pendsv();
    }

    hal_exit_critical(crit);
}

tcb_t *sched_get_current(void) {
    return scheduler.current_task;
}

tcb_t *sched_get_highest_ready(void) {
    int highest_prio = find_highest_prio();
    if (highest_prio < 0) {
        return NULL;
    }

    return ready_list_get_head((uint8_t)highest_prio);
}

int sched_need_switch(void) {
    return scheduler.need_resched;
}

uint32_t sched_get_tick_count(void) {
    return scheduler.tick_count;
}

/*============================================================================
 * 时钟滴答处理
 *============================================================================*/

void sched_tick_handler(void) {
    scheduler.tick_count++;

    tcb_t *current = scheduler.current_task;
    
    // 空闲任务不处理时间片
    if (current && current->id >= 0) {
#if KERN_TIME_SLICE
        if (current->time_slice > 0) {
            current->time_slice--;

            if (current->time_slice == 0) {
                scheduler.need_resched = 1;
                hal_trigger_pendsv();
            }
        }
#endif
    }

    // 检查超时唤醒
    task_id_t id = -1;
    while ((id = task_get_next(id)) != KERN_INVALID_ID) {
        tcb_t *tcb = task_get_tcb(id);
        if (tcb && tcb->state == TASK_STATE_BLOCKED &&
            tcb->wake_tick > 0 &&
            tcb->wake_tick <= scheduler.tick_count) {
            sched_wakeup(tcb, KERN_ERR_TIMEOUT);
        }
    }

#if KERN_TASK_STATS
    sched_update_stats();
#endif
}

#if KERN_TASK_STATS

uint32_t sched_get_cpu_usage(tcb_t *tcb) {
    return tcb->cpu_usage;
}

void sched_update_stats(void) {
    if (scheduler.tick_count - scheduler.last_stat_tick < 100) {
        return;
    }

    scheduler.last_stat_tick = scheduler.tick_count;

    tcb_t *current = scheduler.current_task;
    if (current) {
        current->total_ticks += 100;
    }
}

#endif

/*============================================================================
 * PendSV 处理 (汇编调用)
 *
 * 调用约定:
 * - 汇编已保存当前任务上下文 (R4-R11) 到 _current_task->sp
 * - 此函数选择下一任务
 * - 返回后汇编恢复 _next_task 的上下文
 *============================================================================*/

void kern_pendsv_handler(void) {
    scheduler.need_resched = 0;

    tcb_t *current = _current_task;

    // 处理当前任务状态转换
    if (current) {
        // 空闲任务特殊处理
        if (current->id < 0) {
            // 空闲任务不加入就绪队列，直接设为 READY
            current->state = TASK_STATE_READY;
        } else {
            // 普通任务状态转换
            switch (current->state) {
            case TASK_STATE_RUNNING:
                // RUNNING -> READY，加入就绪队列尾部（时间片轮转）
                current->time_slice = current->time_slice_reload;
                current->state = TASK_STATE_READY;
                ready_list_add_internal(current);
                break;

            case TASK_STATE_TERMINATED:
                // 自动回收：释放任务 ID
                // 注意：TCB 和栈在 task.c 中管理
                {
                    extern void task_reclaim(tcb_t *tcb);
                    task_reclaim(current);
                }
                break;

            case TASK_STATE_BLOCKED:
            case TASK_STATE_SUSPENDED:
                // 不加入就绪队列
                break;

            default:
                // 未知状态，打印警告
                break;
            }
        }
    }

    // 选择下一个任务
    tcb_t *next = sched_get_highest_ready();

    if (next == NULL) {
        // 没有就绪任务，切换到 idle
        next = task_get_idle();
    } else {
        ready_list_remove_internal(next);
    }

    next->state = TASK_STATE_RUNNING;
    scheduler.current_task = next;
    _current_task = next;
    _next_task = next;

#if KERN_TASK_STATS
    if (current && current->id >= 0) {
        current->ctx_switch_count++;
    }
    if (next->id >= 0) {
        next->ctx_switch_count++;
    }
#endif
}
```

- [ ] **Step 2: Commit**

```bash
git add src/kernel/core/scheduler.c
git commit -m "feat(scheduler): rewrite with separated design"
```

---

## Task 5: Update Task Management

**Files:**
- Modify: `src/kernel/task/task.c`

- [ ] **Step 1: Add task_reclaim function**

Add after `task_get_used_bitmap`:

```c
/*============================================================================
 * 任务回收 (由 PendSV 调用)
 *============================================================================*/

void task_reclaim(tcb_t *tcb) {
    if (tcb == NULL || tcb->id < 0) return;  // 空闲任务不回收
    
    task_id_t id = tcb->id;
    
    // 释放任务 ID
    free_task_id(id);
    
    // 清零 TCB (可选，调试时可以保留)
    // memset(tcb, 0, sizeof(tcb_t));
    
    // 栈空间标记空闲 (静态池，实际无需操作)
}
```

- [ ] **Step 2: Add stack overflow check**

Add after `task_reclaim`:

```c
/*============================================================================
 * 栈溢出检测
 *============================================================================*/

void task_check_stack_overflow(void) {
    for (task_id_t id = 0; id < KERN_MAX_TASKS; id++) {
        if (!(task_used_bitmap & (1U << id))) continue;
        
        tcb_t *tcb = &task_pool[id];
        if (tcb->state == TASK_STATE_TERMINATED) continue;
        
        uint8_t *stack_base = (uint8_t *)tcb->stack_base;
        
        // 检查栈底魔数是否被破坏
        int overflow = 0;
        for (int i = 0; i < 16; i++) {
            if (stack_base[i] != STACK_MAGIC_BYTE) {
                overflow = 1;
                break;
            }
        }
        
        if (overflow) {
            hal_debug_puts("\r\n[STACK OVERFLOW] Task: ");
            hal_debug_puts(tcb->name);
            hal_debug_puts("\r\n");
        }
    }
}
```

- [ ] **Step 3: Update idle task to check stack overflow**

Modify `idle_task_func`:

```c
// 空闲任务函数
static void idle_task_func(void *arg) {
    (void)arg;
    uint32_t check_counter = 0;

    while (1) {
#if KERN_IDLE_SLEEP
        hal_enter_lowpower();
#endif

        // 定期检查栈溢出
        if (++check_counter >= 1000) {
            check_counter = 0;
            task_check_stack_overflow();
        }

#if KERN_WATCHDOG_ENABLE
        hal_watchdog_feed();
#endif
    }
}
```

- [ ] **Step 4: Commit**

```bash
git add src/kernel/task/task.c
git commit -m "feat(task): add task reclamation and stack overflow detection"
```

---

## Task 6: Update Scheduler Header

**Files:**
- Modify: `src/kernel/core/scheduler.h`

- [ ] **Step 1: Add new function declarations**

```c
/**
 * @file scheduler.h
 * @brief 调度器接口
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "kernel_types.h"

/*============================================================================
 * 调度器接口
 *============================================================================*/

/**
 * @brief 初始化调度器
 */
void sched_init(void);

/**
 * @brief 启动调度器 (开始运行第一个任务)
 */
void sched_start(void) __attribute__((noreturn));

/**
 * @brief 执行调度 (选择下一任务)
 */
void sched_yield(void);

/**
 * @brief 将任务加入就绪队列
 * @param tcb 任务控制块
 */
void sched_add_ready(tcb_t *tcb);

/**
 * @brief 将任务从就绪队列移除
 * @param tcb 任务控制块
 */
void sched_remove_ready(tcb_t *tcb);

/**
 * @brief 阻塞当前任务
 * @param reason 阻塞原因
 * @param obj 阻塞对象
 * @param timeout 超时 (ticks), 0 表示无限等待
 * @return 操作结果
 */
kern_err_t sched_block(block_reason_t reason, void *obj, uint32_t timeout);

/**
 * @brief 唤醒任务
 * @param tcb 任务控制块
 * @param result 唤醒结果
 */
void sched_wakeup(tcb_t *tcb, kern_err_t result);

/**
 * @brief 获取当前任务
 * @return 当前任务 TCB
 */
tcb_t *sched_get_current(void);

/**
 * @brief 获取最高优先级就绪任务
 * @return 任务 TCB
 */
tcb_t *sched_get_highest_ready(void);

/**
 * @brief 检查是否需要调度
 * @return 非零表示需要调度
 */
int sched_need_switch(void);

/**
 * @brief 时钟滴答处理
 */
void sched_tick_handler(void);

/**
 * @brief 获取 tick 计数
 * @return tick 计数
 */
uint32_t sched_get_tick_count(void);

/*============================================================================
 * 调度器统计接口
 *============================================================================*/

#if KERN_TASK_STATS

/**
 * @brief 获取任务 CPU 使用率
 * @param tcb 任务控制块
 * @return CPU 使用率 (万分比)
 */
uint32_t sched_get_cpu_usage(tcb_t *tcb);

/**
 * @brief 更新 CPU 使用率统计
 */
void sched_update_stats(void);

#endif

#endif // SCHEDULER_H
```

- [ ] **Step 2: Commit**

```bash
git add src/kernel/core/scheduler.h
git commit -m "feat(scheduler): update header with new interfaces"
```

---

## Task 7: Update Kernel Tick Handler

**Files:**
- Modify: `src/kernel/kernel.c`

- [ ] **Step 1: Add kern_tick_handler**

Find or create `kernel.c` and add:

```c
/**
 * @file kernel.c
 * @brief 内核核心
 */

#include "scheduler.h"

void kern_tick_handler(void) {
    sched_tick_handler();
}
```

- [ ] **Step 2: Commit**

```bash
git add src/kernel/kernel.c
git commit -m "feat(kernel): add kern_tick_handler"
```

---

## Task 8: Update Main Test

**Files:**
- Modify: `src/app/main.c`

- [ ] **Step 1: Create basic test**

```c
/**
 * @file main.c
 * @brief 测试程序
 */

#include "kernel.h"
#include "task.h"
#include "hal.h"

// 测试任务 1
static void task1_func(void *arg) {
    (void)arg;
    int count = 0;
    
    while (1) {
        hal_debug_puts("\r\n[Task1] count=");
        hal_debug_putc('0' + (count++ % 10));
        task_delay(500);
    }
}

// 测试任务 2
static void task2_func(void *arg) {
    (void)arg;
    int count = 0;
    
    while (1) {
        hal_debug_puts("\r\n[Task2] count=");
        hal_debug_putc('0' + (count++ % 10));
        task_delay(1000);
    }
}

// 测试任务 3 (高优先级，测试抢占)
static void task3_func(void *arg) {
    (void)arg;
    
    while (1) {
        hal_debug_puts("\r\n[Task3] HIGH PRIORITY RUNNING");
        task_delay(2000);
    }
}

int main(void) {
    // 初始化 HAL
    hal_cpu_init();
    
    // 初始化内核
    task_init();
    sched_init();
    
    // 创建测试任务
    task_id_t t1 = task_create("task1", task1_func, NULL, 10, 0);
    task_id_t t2 = task_create("task2", task2_func, NULL, 20, 0);
    task_id_t t3 = task_create("task3", task3_func, NULL, 5, 0);  // 高优先级
    
    // 启动任务
    task_start(t1);
    task_start(t2);
    task_start(t3);
    
    hal_debug_puts("\r\n[Main] Starting scheduler...\r\n");
    
    // 启动调度器
    sched_start();
    
    // 不应该到达这里
    while (1);
    
    return 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/app/main.c
git commit -m "test: add basic scheduler test"
```

---

## Task 9: Build and Test

- [ ] **Step 1: Build the project**

```bash
cd /home/five/my-rtos
make clean
make
```

Expected: Build succeeds without errors

- [ ] **Step 2: Flash and test**

```bash
make flash
```

Expected: Tasks run correctly, no HardFault

---

## Summary

| Task | Description | Files |
|------|-------------|-------|
| 1 | Update HAL Interface | hal.h |
| 2 | Update HAL Implementation | hal.c |
| 3 | Rewrite PendSV Handler | context.S |
| 4 | Rewrite Scheduler Core | scheduler.c |
| 5 | Update Task Management | task.c |
| 6 | Update Scheduler Header | scheduler.h |
| 7 | Update Kernel Tick Handler | kernel.c |
| 8 | Update Main Test | main.c |
| 9 | Build and Test | - |
