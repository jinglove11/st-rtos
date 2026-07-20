/**
 * @file scheduler.c
 * @brief 调度器实现 - 分离式设计
 *
 * ============================================================================
 * 设计原则
 * ============================================================================
 *
 * 本调度器采用"决策与执行分离"的设计模式：
 *
 * 1. 调度器核心（C 代码）只负责"决策"
 *    - 维护就绪队列
 *    - 选择下一个运行的任务
 *    - 处理任务状态转换
 *
 * 2. PendSV（汇编代码）负责"执行"
 *    - 保存当前任务的寄存器（R4-R11）
 *    - 恢复下一个任务的寄存器
 *    - 完成上下文切换
 *
 * 这种分离设计的优点：
 * - C 代码易于理解和维护
 * - 汇编代码只处理硬件相关的上下文操作
 * - 避免了 C 函数调用栈与 PendSV 保存位置冲突的问题
 *
 * ============================================================================
 * 调度策略
 * ============================================================================
 *
 * 1. 优先级抢占调度
 *    - 高优先级任务总是抢占低优先级任务
 *    - 优先级数值越小，优先级越高（0 为最高优先级）
 *
 * 2. 时间片轮转（可选）
 *    - 同优先级任务按时间片轮转执行
 *    - 时间片用完后任务被放回就绪队列尾部
 *
 * 3. O(1) 时间复杂度
 *    - 使用位图快速查找最高优先级任务
 *    - 就绪队列使用双向链表，插入/删除都是 O(1)
 *
 * ============================================================================
 */

#include "scheduler.h"
#include "kernel_config.h"
#include "kernel_types.h"
#include "task.h"
#include "hal.h"
#include "spinlock.h"
#include <string.h>

#if TRACE_ENABLE
#include "trace.h"
#endif
#if KERN_TASK_STATS
#include "stats.h"
#endif

/* 外部函数声明 */
extern tcb_t *task_get_tcb(task_id_t task_id);
extern task_id_t task_get_next(task_id_t task_id);
extern uint64_t task_get_used_bitmap(void);

#if KERN_DEBUG_ENABLE
extern void kern_panic(const char *msg);
#endif

/*============================================================================
 * 全局变量 - 汇编访问
 *============================================================================*/

/**
 * @brief 当前运行任务的 TCB 指针
 *
 * 这个变量由 PendSV 汇编代码直接访问：
 * - PendSV 保存上下文时，读取此指针获取当前任务的 TCB
 * - 首次启动调度器时，此值为 NULL（SVC 处理程序的特殊情况）
 *
 * SMP: indexed by CPU id. _current_task[hal_get_cpu_id()].
 */
tcb_t *volatile _current_task[SMP_MAX_CPUS] = { NULL };

/**
 * @brief 下一个要运行的任务 TCB 指针
 *
 * 这个变量由 PendSV 汇编代码直接访问：
 * - kern_pendsv_handler() 设置此指针
 * - PendSV 恢复上下文时，读取此指针获取下一个任务的 TCB
 *
 * SMP: indexed by CPU id.
 */
tcb_t *volatile _next_task[SMP_MAX_CPUS] = { NULL };

/*============================================================================
 * 内部数据结构
 *============================================================================*/

/**
 * @brief 就绪队列结构（每个优先级一个双向链表）
 *
 * 使用双向链表的原因：
 * - 插入到尾部：O(1)
 * - 从头部取出：O(1)
 * - 从任意位置删除：O(1)（已知节点指针时）
 */
typedef struct {
    tcb_t *head;    /**< 链表头（最先被选中的任务） */
    tcb_t *tail;    /**< 链表尾（最新加入的任务） */
} ready_list_t;

/**
 * @brief 调度器全局状态
 *
 * 包含调度器运行所需的所有状态信息。
 */
static struct {
    tcb_t *current_task;                        /**< 当前运行的任务 (shadow of _current_task) */
    ready_list_t ready_list[KERNEL_MAX_PRIORITIES]; /**< 每个优先级的就绪队列 (共享) */
    volatile uint32_t ready_bitmap[4];          /**< 优先级位图（128级优先级） */
    volatile uint32_t tick_count;               /**< 系统滴答计数 (共享) */
    volatile int need_resched[SMP_MAX_CPUS];    /**< per-CPU 重调度标志 */
    int started;                                /**< 调度器是否已启动 */
} scheduler;

/* Spinlock protecting the shared ready_list / ready_bitmap / tick_count.
 * In single-core mode this degrades to IRQ disable (irq_spin_lock does
 * PRIMASK save + spinlock, but spinlock is uncontended). */
static irq_spinlock_t sched_lock;

/*============================================================================
 * 位图操作 - 快速查找最高优先级
 *============================================================================*/

/**
 * @brief 查找当前最高优先级
 *
 * 使用位图实现 O(1) 时间复杂度的优先级查找。
 *
 * 位图结构：
 * - ready_bitmap[0]: 优先级 0-31（最高优先级组）
 * - ready_bitmap[1]: 优先级 32-63
 * - ready_bitmap[2]: 优先级 64-95
 * - ready_bitmap[3]: 优先级 96-127（最低优先级组）
 *
 * @return 最高优先级值，如果没有就绪任务则返回 -1
 *
 * @note __builtin_ctz() 是 GCC 内置函数，计算从最低位开始的连续零的个数
 *       例如：0b00010000 返回 4（第 4 位是第一个 1）
 */
static inline int find_highest_prio(void) {
    for (int i = 0; i < 4; i++) {
        if (scheduler.ready_bitmap[i] != 0) {
            /* 找到第一个非空的位图组，计算优先级 */
            return i * 32 + __builtin_ctz(scheduler.ready_bitmap[i]);
        }
    }
    return -1;  /* 没有就绪任务 */
}

/**
 * @brief 设置位图中对应优先级的位
 * @param prio 优先级值（0-127）
 */
static inline void bitmap_set(uint8_t prio) {
    scheduler.ready_bitmap[prio / 32] |= (1U << (prio % 32));
}

/**
 * @brief 清除位图中对应优先级的位
 * @param prio 优先级值（0-127）
 */
static inline void bitmap_clear(uint8_t prio) {
    scheduler.ready_bitmap[prio / 32] &= ~(1U << (prio % 32));
}

#if KERN_DEBUG_ENABLE
static int ready_bitmap_has(uint8_t prio) {
    return (scheduler.ready_bitmap[prio / 32] & (1U << (prio % 32))) != 0;
}

static int ready_list_validate_prio(uint8_t prio) {
    ready_list_t *list = &scheduler.ready_list[prio];

    if ((list->head == NULL) != (list->tail == NULL)) {
        return 0;
    }

    if (ready_bitmap_has(prio) != (list->head != NULL)) {
        return 0;
    }

    if (list->head && list->head->prev != NULL) {
        return 0;
    }

    if (list->tail && list->tail->next != NULL) {
        return 0;
    }

    uint16_t count = 0;
    tcb_t *prev = NULL;
    tcb_t *curr = list->head;

    while (curr) {
        if (curr->prev != prev) {
            return 0;
        }

        if (curr->state != TASK_STATE_READY || curr->priority != prio) {
            return 0;
        }

        prev = curr;
        curr = curr->next;
        count++;

        if (count > KERNEL_MAX_TASKS + 1) {
            return 0;
        }
    }

    return prev == list->tail;
}

static void ready_list_check_prio(uint8_t prio, const char *where) {
    if (!ready_list_validate_prio(prio)) {
        hal_debug_puts("\r\n[SCHED] ready queue invariant failed at ");
        hal_debug_puts(where);
        hal_debug_puts("\r\n");
        kern_panic("ready queue invariant");
    }
}
#else
#define ready_list_check_prio(prio, where) do { (void)(prio); } while (0)
#endif

/*============================================================================
 * 就绪队列操作 - 内部函数
 *============================================================================*/

/**
 * @brief 将任务添加到就绪队列尾部
 *
 * 添加到尾部的原因：
 * - 实现时间片轮转：先加入的任务先被选中
 * - 同优先级任务按 FIFO 顺序执行
 *
 * @param tcb 任务控制块指针
 *
 * @note 调用者必须持有临界区锁
 */
static void ready_list_add_internal(tcb_t *tcb) {
    uint8_t prio = tcb->priority;
    ready_list_t *list = &scheduler.ready_list[prio];

    /* 设置链表节点指针 */
    tcb->next = NULL;
    tcb->prev = list->tail;

    /* 更新链表 */
    if (list->tail) {
        /* 链表非空，添加到尾部 */
        list->tail->next = tcb;
    } else {
        /* 链表为空，作为唯一节点 */
        list->head = tcb;
    }
    list->tail = tcb;

    /* 设置位图，表示该优先级有就绪任务 */
    bitmap_set(prio);
    ready_list_check_prio(prio, "add");
}

/**
 * @brief 从就绪队列中移除任务
 *
 * @param tcb 任务控制块指针
 *
 * @note 调用者必须持有临界区锁
 * @note 如果移除后链表为空，会清除位图中对应的位
 */
static int ready_list_remove_from_prio_internal(tcb_t *tcb, uint8_t prio) {
    if (prio >= KERNEL_MAX_PRIORITIES) {
        return 0;
    }

    ready_list_t *list = &scheduler.ready_list[prio];
    tcb_t *iter = list->head;

    while (iter && iter != tcb) {
        iter = iter->next;
    }

    if (iter != tcb) {
        return 0;
    }

    /* 更新前驱节点的 next 指针 */
    if (tcb->prev) {
        tcb->prev->next = tcb->next;
    } else {
        /* 没有前驱节点，说明是链表头 */
        list->head = tcb->next;
    }

    /* 更新后继节点的 prev 指针 */
    if (tcb->next) {
        tcb->next->prev = tcb->prev;
    } else {
        /* 没有后继节点，说明是链表尾 */
        list->tail = tcb->prev;
    }

    /* 清除节点的链表指针 */
    tcb->next = NULL;
    tcb->prev = NULL;

    /* 如果链表为空，清除位图 */
    if (list->head == NULL) {
        bitmap_clear(prio);
    }

    ready_list_check_prio(prio, "remove");
    return 1;
}

static int ready_list_contains_any_internal(tcb_t *tcb) {
    if (tcb == NULL) {
        return 0;
    }

    for (uint8_t prio = 0; prio < KERNEL_MAX_PRIORITIES; prio++) {
        ready_list_t *list = &scheduler.ready_list[prio];
        tcb_t *iter = list->head;

        while (iter) {
            if (iter == tcb) {
                return 1;
            }
            iter = iter->next;
        }
    }

    return 0;
}

static void ready_list_remove_internal(tcb_t *tcb) {
    uint8_t prio = tcb->priority;

    if (ready_list_remove_from_prio_internal(tcb, prio)) {
        return;
    }

    /*
     * Priority inheritance may update tcb->priority before asking the scheduler
     * to reinsert the task. Fall back to an exact pointer search so stale queue
     * membership is removed from the old priority list.
     */
    for (uint8_t p = 0; p < KERNEL_MAX_PRIORITIES; p++) {
        if (p == prio) {
            continue;
        }
        if (ready_list_remove_from_prio_internal(tcb, p)) {
            return;
        }
    }
}

/**
 * @brief 获取指定优先级就绪队列的头节点
 * @param prio 优先级值
 * @return 队列头节点，如果队列为空则返回 NULL
 */
static tcb_t *ready_list_get_head(uint8_t prio) {
    (void)prio;  /* 参数用于调试，避免未使用警告 */
    return scheduler.ready_list[prio].head;
}

/*============================================================================
 * 公开接口实现 - 初始化与启动
 *============================================================================*/

/**
 * @brief 初始化调度器
 *
 * 初始化内容：
 * - 清空所有就绪队列
 * - 清零位图
 * - 重置调度器状态
 *
 * @note 必须在创建任何任务之前调用
 */
void sched_init(void) {
    irq_spin_init(&sched_lock);

    uint32_t crit = irq_spin_lock(&sched_lock);

    memset((void *)&scheduler, 0, sizeof(scheduler));
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        _current_task[cpu] = NULL;
        _next_task[cpu] = NULL;
    }

    irq_spin_unlock(&sched_lock, crit);
}

/**
 * @brief 启动调度器
 *
 * 启动流程：
 * 1. 检查是否有任务可运行
 * 2. 选择最高优先级任务作为第一个运行的任务
 * 3. 启动 SysTick 定时器
 * 4. 使能全局中断
 * 5. 触发 SVC 异常，开始第一个任务的执行
 *
 * @note 此函数不会返回
 * @note 必须在调用 kern_init() 和创建至少一个任务后调用
 */
void sched_start(void) {
    uint32_t crit = irq_spin_lock(&sched_lock);

    /* 检查是否有任务可运行 */
    int has_task = 0;
    for (int i = 0; i < 4; i++) {
        if (scheduler.ready_bitmap[i] != 0) {
            has_task = 1;
            break;
        }
    }

    if (!has_task) {
        irq_spin_unlock(&sched_lock, crit);
        hal_debug_puts("[SCHED] No task to run!\r\n");
        /* 没有任务，进入低功耗模式 */
        while (1) {
            hal_enter_lowpower();
        }
    }

    /* 获取第一个要运行的任务 */
    tcb_t *first = sched_get_highest_ready();
    if (first == NULL) {
        irq_spin_unlock(&sched_lock, crit);
        hal_debug_puts("[SCHED] get_highest_ready returned NULL!\r\n");
        while (1) {
            hal_enter_lowpower();
        }
    }

    /* 从就绪队列移除（即将变为 RUNNING 状态） */
    ready_list_remove_internal(first);

    /* 设置为运行状态 */
    first->state = TASK_STATE_RUNNING;

    /*
     * 关键：设置 _current_task = NULL
     * SVC 处理程序会检查这个值，如果是 NULL 则跳过上下文保存
     * 这是首次切换的特殊情况
     */
    uint32_t cpu = hal_get_cpu_id();
    _current_task[cpu] = NULL;
    _next_task[cpu] = first;
    /* 不再设 scheduler.current_task (per-CPU 用 _current_task) */

    scheduler.started = 1;

    irq_spin_unlock(&sched_lock, crit);

    /* 启动 SysTick 定时器 */
    hal_systick_init(KERNEL_TICK_RATE);

    /* 使能全局中断 */
    hal_irq_enable();

    /*
     * 触发 SVC 异常
     * SVC 处理程序会：
     * 1. 从 _next_task 获取第一个任务
     * 2. 恢复任务的上下文
     * 3. 切换到线程模式，开始执行任务
     */
    hal_trigger_first_switch();

    /* 不应该到达这里 */
    while (1);
}

/*============================================================================
 * 公开接口实现 - 调度控制
 *============================================================================*/

/**
 * @brief 主动让出 CPU
 *
 * 当前任务主动放弃 CPU 使用权，触发调度器选择下一个任务运行。
 *
 * 使用场景：
 * - 任务完成当前工作，等待新事件
 * - 实现协作式多任务
 * - 低优先级任务让出 CPU 给高优先级任务
 *
 * @note 当前任务会被放回就绪队列尾部
 */
void sched_yield(void) {
    scheduler.need_resched[hal_get_cpu_id()] = 1;
    hal_trigger_pendsv();
}

/**
 * @brief 将任务加入就绪队列
 *
 * 当任务变为可运行状态时调用：
 * - task_start() 启动任务
 * - task_resume() 恢复挂起任务
 * - sched_wakeup() 唤醒阻塞任务
 *
 * @param tcb 任务控制块指针
 *
 * @note 如果任务优先级高于当前运行任务，会触发调度
 */
void sched_add_ready(tcb_t *tcb) {
    uint32_t crit = irq_spin_lock(&sched_lock);

    /* 已经在就绪队列中则不重复添加 */
    if (ready_list_contains_any_internal(tcb)) {
        tcb->state = TASK_STATE_READY;
        irq_spin_unlock(&sched_lock, crit);
        return;
    }

    /* 更新状态并加入就绪队列 */
    tcb->state = TASK_STATE_READY;
    ready_list_add_internal(tcb);

    /*
     * 抢占检查：先标记,解锁后再触发 PendSV。
     * 不能在持锁时触发 PendSV (SMP 下 PendSV handler 会等锁,死锁)。
     */
    int need_preempt = 0;
    if (scheduler.started) {
        tcb_t *curr = sched_get_current();
        if (curr && tcb->priority < curr->priority) {
            need_preempt = 1;
        }
    }

    irq_spin_unlock(&sched_lock, crit);

    if (need_preempt) {
        scheduler.need_resched[hal_get_cpu_id()] = 1;
        hal_trigger_pendsv();
    }
}

/**
 * @brief 从就绪队列移除任务
 *
 * 当任务变为不可运行状态时调用：
 * - task_suspend() 挂起任务
 * - task_delete() 删除任务
 *
 * @param tcb 任务控制块指针
 */
void sched_remove_ready(tcb_t *tcb) {
    uint32_t crit = irq_spin_lock(&sched_lock);

    if (!ready_list_contains_any_internal(tcb)) {
        irq_spin_unlock(&sched_lock, crit);
        return;
    }

    ready_list_remove_internal(tcb);
    irq_spin_unlock(&sched_lock, crit);
}

/**
 * @brief 重新插入就绪队列（优先级变化时）
 *
 * 当任务的优先级发生变化时，需要重新插入到对应优先级的队列中。
 * 主要用于优先级继承机制。
 *
 * @param tcb 任务控制块指针
 *
 * @note 此函数在临界区内被调用，不嵌套临界区
 */
void sched_reinsert_by_priority(tcb_t *tcb) {
    if (!ready_list_contains_any_internal(tcb)) {
        return;
    }

    /* 先移除再重新插入 */
    ready_list_remove_internal(tcb);
    ready_list_add_internal(tcb);
}

/*============================================================================
 * 公开接口实现 - 任务阻塞与唤醒
 *============================================================================*/

/**
 * @brief 阻塞当前任务
 *
 * 将当前任务从就绪队列移除，并设置阻塞原因和超时时间。
 * 任务将暂停执行，直到被唤醒或超时。
 *
 * @param reason 阻塞原因（SLEEP、MUTEX、SEM 等）
 * @param obj    阻塞对象（互斥锁、信号量等）的指针
 * @param timeout 超时时间（滴答数），0 表示无限等待
 * @return 唤醒时的结果码
 *
 * 使用场景：
 * - task_delay() 延时
 * - mutex_lock() 等待互斥锁
 * - sem_wait() 等待信号量
 *
 * @note 此函数会触发调度，当前任务暂停执行
 * @note 当任务被唤醒后，会从调用点继续执行
 */
kern_err_t sched_block(block_reason_t reason, void *obj, uint32_t timeout) {
    tcb_t *current = sched_get_current();

    if (current == NULL) {
        return KERN_ERR_STATE;
    }

    uint32_t crit = irq_spin_lock(&sched_lock);

    /* 从就绪队列移除 */
    if (current->state == TASK_STATE_READY) {
        ready_list_remove_internal(current);
    }

    /* 设置阻塞状态 */
    current->state = TASK_STATE_BLOCKED;
    current->block_reason = reason;
    current->block_obj = obj;
    current->block_result = KERN_OK;

    /* 设置超时唤醒时间 */
    if (timeout > 0) {
        current->wake_tick = scheduler.tick_count + timeout;
    } else {
        current->wake_tick = 0;
    }

    scheduler.need_resched[hal_get_cpu_id()] = 1;

    /*
     * 解锁并触发 PendSV
     * 注意顺序：先解锁，再触发 PendSV
     * 这样 PendSV 可以在临界区外执行
     */
    irq_spin_unlock(&sched_lock, crit);
    hal_trigger_pendsv();

    /*
     * 当任务被唤醒后，会从这里继续执行
     * block_result 已经由 sched_wakeup() 设置
     */
    return current->block_result;
}

/**
 * @brief 唤醒阻塞的任务
 *
 * 将阻塞的任务重新加入就绪队列，使其可以继续执行。
 *
 * @param tcb    任务控制块指针
 * @param result 唤醒结果码（KERN_OK 或 KERN_ERR_TIMEOUT）
 *
 * @note 如果唤醒的任务优先级高于当前任务，会触发调度
 */
void sched_wakeup(tcb_t *tcb, kern_err_t result) {
    uint32_t crit = irq_spin_lock(&sched_lock);

    /* 确保任务处于阻塞状态 */
    if (tcb->state != TASK_STATE_BLOCKED) {
        irq_spin_unlock(&sched_lock, crit);
        return;
    }

    /* 清除阻塞信息 (block_obj 保留，由 IPC 原语负责清理等待队列) */
    tcb->block_reason = BLOCK_REASON_NONE;
    tcb->block_result = result;
    tcb->wake_tick = 0;
    task_complete_blocked_syscall(tcb, result);

    /* 加入就绪队列 */
    tcb->state = TASK_STATE_READY;
    ready_list_add_internal(tcb);

    /* 抢占检查:先标记,解锁后再触发 PendSV (避免持锁死锁) */
    int need_preempt = 0;
    tcb_t *current = sched_get_current();
    if (current && tcb->priority < current->priority) {
        need_preempt = 1;
    }

    irq_spin_unlock(&sched_lock, crit);

    if (need_preempt) {
        scheduler.need_resched[hal_get_cpu_id()] = 1;
        hal_trigger_pendsv();
    }
}

/*============================================================================
 * 公开接口实现 - 状态查询
 *============================================================================*/

/**
 * @brief 获取当前运行任务的 TCB
 * @return 当前任务的 TCB 指针，如果没有任务运行则返回 NULL
 */
tcb_t *sched_get_current(void) {
    /* 统一用 per-cpu _current_task (单核下 cpu=0,等价)。
     * scheduler.current_task 全局变量已废弃 (SMP 下两核并发写竞态)。 */
    return _current_task[hal_get_cpu_id()];
}

/**
 * @brief 通过任务 ID 获取 TCB
 * @param task_id 任务 ID
 * @return 任务 TCB 指针，如果任务不存在则返回 NULL
 */
tcb_t *sched_get_tcb(task_id_t task_id) {
    extern tcb_t *task_get_tcb(task_id_t task_id);
    return task_get_tcb(task_id);
}

/**
 * @brief 获取最高优先级的就绪任务
 *
 * 查找流程：
 * 1. 使用位图快速找到最高优先级
 * 2. 获取该优先级就绪队列的头节点
 * 3. 检查 TCB 是否有效（防止已回收任务被选中）
 *
 * @return 最高优先级就绪任务的 TCB，如果没有就绪任务则返回 NULL
 */
tcb_t *sched_get_highest_ready(void) {
    int highest_prio = find_highest_prio();
    if (highest_prio < 0) {
        return NULL;
    }

    tcb_t *tcb = ready_list_get_head((uint8_t)highest_prio);

    /*
     * 安全检查：跳过无效的 TCB
     *
     * 问题场景：任务被回收后，TCB 被清零，但可能仍在就绪队列链表中
     * 解决方案：检查任务名称是否为空，如果为空则跳过
     */
    while (tcb && tcb->name[0] == '\0') {
        /* 清除无效的就绪队列 */
        bitmap_clear((uint8_t)highest_prio);
        scheduler.ready_list[highest_prio].head = NULL;
        scheduler.ready_list[highest_prio].tail = NULL;

        /* 重新查找 */
        highest_prio = find_highest_prio();
        if (highest_prio < 0) {
            return NULL;
        }
        tcb = ready_list_get_head((uint8_t)highest_prio);
    }

    return tcb;
}

/**
 * @brief 检查是否需要调度
 * @return 1 表示需要调度，0 表示不需要
 */
int sched_need_switch(void) {
    return scheduler.need_resched[hal_get_cpu_id()];
}

/**
 * @brief 获取系统滴答计数
 * @return 当前滴答计数
 */
uint32_t sched_get_tick_count(void) {
    return scheduler.tick_count;
}

/*============================================================================
 * 时钟滴答处理
 *============================================================================*/

/**
 * @brief 时钟滴答处理函数
 *
 * 由 SysTick 中断调用，处理以下任务：
 * 1. 更新系统滴答计数
 * 2. 处理时间片轮转
 * 3. 检查超时唤醒
 *
 * @note 在中断上下文中调用
 */
void sched_tick_handler(void) {
    /* 根因4:原子递增 tick_count (两核 SysTick 同时++) */
    __asm volatile("1: ldrex r2, [%0]\n"
                   "   adds  r2, #1\n"
                   "   strex r1, r2, [%0]\n"
                   "   cmp   r1, #0\n"
                   "   bne   1b\n"
                   :: "r"(&scheduler.tick_count) : "r1", "r2", "memory");

    tcb_t *current = sched_get_current();

    /*
     * 时间片处理（空闲任务不参与时间片轮转）
     *
     * 时间片轮转机制：
     * - 每个滴答，时间片计数器减 1
     * - 计数器为 0 时，触发调度
     * - 任务被放回就绪队列尾部，让出 CPU 给同优先级任务
     */
    if (current && current->id >= 0) {
#if KERN_TIME_SLICE
        if (current->time_slice > 0) {
            current->time_slice--;

            if (current->time_slice == 0) {
                scheduler.need_resched[hal_get_cpu_id()] = 1;
                hal_trigger_pendsv();
            }
        }
#endif
    } else {
        /* 当前是 idle (id<0) 或 NULL:检查 ready 队列是否有任务。
         * 如果有,触发 PendSV 切到 ready 任务。
         * 不加锁读 ready_bitmap (只是提示 PendSV,PendSV handler 会加锁重新检查)。
         * 这修复了"idle 卡死":之前 idle 不参与时间片,
         * ready 队列里有任务但没人触发重调度。 */
        int has_ready = 0;
        for (int i = 0; i < 4; i++) {
            if (scheduler.ready_bitmap[i] != 0) {
                has_ready = 1;
                break;
            }
        }
        if (has_ready) {
            scheduler.need_resched[hal_get_cpu_id()] = 1;
            hal_trigger_pendsv();
        }
    }

    /*
     * 超时唤醒检查
     *
     * 遍历所有任务，检查是否有阻塞任务超时
     * 如果 wake_tick <= 当前滴答，则唤醒该任务
     */
    uint64_t used_snapshot = task_get_used_bitmap();
    for (task_id_t id = 0; id < KERNEL_MAX_TASKS; id++) {
        if ((used_snapshot & (1ULL << id)) == 0) {
            continue;
        }

        tcb_t *tcb = task_get_tcb(id);
        if (tcb && tcb->state == TASK_STATE_BLOCKED &&
            tcb->wake_tick > 0 &&
            tcb->wake_tick <= scheduler.tick_count) {
            kern_err_t wake_result =
                (tcb->block_reason == BLOCK_REASON_SLEEP) ? KERN_OK :
                                                            KERN_ERR_TIMEOUT;
            if (tcb->syscall_blocked) {
                (void)task_cancel_blocked_wait(tcb);
            }
            sched_wakeup(tcb, wake_result);
        }
    }

    /* 回收已过期的终止任务 (延迟一拍，给 task_join 时间读取 exit_value) */
    task_reclaim_expired();

#if KERN_TASK_STATS
    stats_tick_update();
#endif
}

/*============================================================================
 * 任务统计（可选功能）
 *============================================================================*/

#if KERN_TASK_STATS

uint32_t sched_get_cpu_usage(tcb_t *tcb) {
    return tcb ? tcb->cpu_usage : 0;
}

#endif

/*============================================================================
 * PendSV 处理程序 - 核心调度逻辑
 *============================================================================*/

/**
 * @brief PendSV 处理程序（C 部分）
 *
 * ============================================================================
 * 调用约定
 * ============================================================================
 *
 * PendSV 汇编代码会调用此函数：
 * 1. 汇编已保存当前任务上下文（R4-R11）到 _current_task->sp
 * 2. 此函数选择下一个任务并设置 _next_task
 * 3. 返回后汇编恢复 _next_task 的上下文
 *
 * ============================================================================
 * 状态转换规则
 * ============================================================================
 *
 * 当前任务状态    →  新状态      →  操作
 * ─────────────────────────────────────────────
 * RUNNING        →  READY       →  加入就绪队列尾部
 * TERMINATED     →  (回收)      →  从就绪队列移除，释放资源
 * BLOCKED        →  BLOCKED     →  不加入就绪队列
 * SUSPENDED      →  SUSPENDED   →  不加入就绪队列
 *
 * ============================================================================
 * 空闲任务特殊处理
 * ============================================================================
 *
 * 空闲任务（id < 0）不加入就绪队列：
 * - 当没有其他任务可运行时，调度器直接切换到空闲任务
 * - 空闲任务状态始终为 READY
 */
void kern_pendsv_handler(void) {
    /* 清除调度标志 */
    scheduler.need_resched[hal_get_cpu_id()] = 0;

    tcb_t *current = _current_task[hal_get_cpu_id()];

    /* 内存屏障：确保读取正确的值 */
    __asm volatile("dmb");

    /* SMP-A 修复:PendSV handler 用 spin_lock 保护 ready_list。
     *
     * 安全性保证:所有持 sched_lock 的路径 (sched_add_ready/sched_wakeup/
     * sched_block/sched_remove_ready) 都在**解锁后**才触发 PendSV,
     * 所以 PendSV 执行时锁一定空闲。单核下 PendSV 是最低优先级异常,
     * 不会被 tick handler 中断 (tick handler 不持锁)。
     *
     * PendSV 已在 cpsid i 下 (汇编入口),用纯 spin_lock (不操作 PRIMASK)。
     */
    spin_lock(&sched_lock.lock);

    /*
     * 处理当前任务状态转换
     */
    if (current) {
        /* 空闲任务特殊处理 */
        if (current->id < 0) {
            /* 空闲任务不加入就绪队列，直接设为 READY */
            current->state = TASK_STATE_READY;
        } else {
            /* 普通任务状态转换 */
            switch (current->state) {

            case TASK_STATE_RUNNING:
                current->time_slice = current->time_slice_reload;
                current->state = TASK_STATE_READY;
                ready_list_add_internal(current);
                break;

            case TASK_STATE_TERMINATED:
                {
                    ready_list_t *list = &scheduler.ready_list[current->priority];
                    if (list->head == current || current->next != NULL || current->prev != NULL) {
                        ready_list_remove_internal(current);
                    }
                    extern void task_reclaim(tcb_t *tcb);
                    task_reclaim(current);
                }
                break;

            case TASK_STATE_BLOCKED:
                break;

            case TASK_STATE_SUSPENDED:
                break;

            default:
                break;
            }
        }
    }

    /*
     * 选择下一个任务
     */
    tcb_t *next = sched_get_highest_ready();

    if (next == NULL || next->name[0] == '\0') {
        next = task_get_idle();
    } else {
        ready_list_remove_internal(next);
    }

    next->state = TASK_STATE_RUNNING;

    spin_unlock(&sched_lock.lock);

    /* 更新 per-CPU 指针 (供汇编加载上下文) */

#if TRACE_ENABLE
    trace_record(TRACE_TASK_SWITCH, (uint8_t)(next->id >= 0 ? next->id : 0), 0);
#endif

    /* 内存屏障：确保写入完成 */
    __asm volatile("dmb");

    /* 更新全局指针，供汇编代码使用 (per-CPU indexed) */
    uint32_t cpu = hal_get_cpu_id();
    _current_task[cpu] = next;
    _next_task[cpu] = next;

#if KERN_TASK_STATS
    stats_task_switch(current, next);
#endif
}
