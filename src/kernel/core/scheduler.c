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
#if CAP_ENABLE
#include "capability.h"
#endif
#if SMP
#include "smp.h"
#endif
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

typedef struct {
    ready_list_t ready_list[KERNEL_MAX_PRIORITIES];
    volatile uint32_t ready_bitmap[4];
    volatile uint32_t ready_count;
    volatile int need_resched;
} runqueue_t;

#if SMP
typedef enum {
    REMOTE_OP_ADD = 1,
    REMOTE_OP_REMOVE,
    REMOTE_OP_REINSERT,
    REMOTE_OP_QUIESCE,
} remote_op_type_t;

typedef struct {
    tcb_t *tcb;
    volatile uint32_t *completion;
    uint8_t op;
    uint8_t _pad[3];
} remote_op_t;

#define SCHED_REMOTE_QUEUE_LEN (KERNEL_MAX_TASKS * 2U)

typedef struct {
    irq_spinlock_t lock;
    remote_op_t entries[SCHED_REMOTE_QUEUE_LEN];
    uint16_t head;
    uint16_t tail;
    uint16_t count;
} remote_queue_t;
#endif

/**
 * @brief 调度器全局状态
 *
 * 包含调度器运行所需的所有状态信息。
 */
static struct {
    runqueue_t runq[SMP_MAX_CPUS];              /**< owner-CPU-only ready queues */
    volatile uint32_t tick_count;               /**< 系统滴答计数 (共享) */
    volatile uint32_t online_mask;              /**< CPUs eligible for placement */
    int started;                                /**< 调度器是否已启动 */
#if SMP
    remote_queue_t remote[SMP_MAX_CPUS];
    volatile uint8_t steal_pending[SMP_MAX_CPUS];
    volatile uint32_t *quiesce_completion[SMP_MAX_CPUS];
#endif
} scheduler;

/* Boot/configuration lock only.  Runtime ready-queue operations are local-IRQ
 * protected or delivered to the owner CPU by IPI; PendSV never takes it. */
static irq_spinlock_t sched_lock;
static tcb_t *timeout_service_tcb;

static inline uint32_t sched_cpu(void) {
    uint32_t cpu = hal_get_cpu_id();
    return (cpu < SMP_MAX_CPUS) ? cpu : 0U;
}

static inline runqueue_t *runq_for(uint32_t cpu) {
    return &scheduler.runq[cpu];
}

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
static inline int find_highest_prio(const runqueue_t *rq) {
    for (int i = 0; i < 4; i++) {
        if (rq->ready_bitmap[i] != 0) {
            /* 找到第一个非空的位图组，计算优先级 */
            return i * 32 + __builtin_ctz(rq->ready_bitmap[i]);
        }
    }
    return -1;  /* 没有就绪任务 */
}

/**
 * @brief 设置位图中对应优先级的位
 * @param prio 优先级值（0-127）
 */
static inline void bitmap_set(runqueue_t *rq, uint8_t prio) {
    rq->ready_bitmap[prio / 32] |= (1U << (prio % 32));
}

/**
 * @brief 清除位图中对应优先级的位
 * @param prio 优先级值（0-127）
 */
static inline void bitmap_clear(runqueue_t *rq, uint8_t prio) {
    rq->ready_bitmap[prio / 32] &= ~(1U << (prio % 32));
}

#if KERN_DEBUG_ENABLE
static int ready_bitmap_has(const runqueue_t *rq, uint8_t prio) {
    return (rq->ready_bitmap[prio / 32] & (1U << (prio % 32))) != 0;
}

static int ready_list_validate_prio(const runqueue_t *rq, uint8_t prio) {
    const ready_list_t *list = &rq->ready_list[prio];

    if ((list->head == NULL) != (list->tail == NULL)) {
        return 0;
    }

    if (ready_bitmap_has(rq, prio) != (list->head != NULL)) {
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

static void ready_list_check_prio(const runqueue_t *rq, uint8_t prio,
                                  const char *where) {
    if (!ready_list_validate_prio(rq, prio)) {
        hal_debug_puts("\r\n[SCHED] ready queue invariant failed at ");
        hal_debug_puts(where);
        hal_debug_puts("\r\n");
        kern_panic("ready queue invariant");
    }
}
#else
#define ready_list_check_prio(rq, prio, where) \
    do { (void)(rq); (void)(prio); } while (0)
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
static void ready_list_add_internal(runqueue_t *rq, uint32_t cpu, tcb_t *tcb) {
    uint8_t prio = tcb->priority;
    ready_list_t *list = &rq->ready_list[prio];

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
    bitmap_set(rq, prio);
    rq->ready_count++;
    tcb->cpu_owner = (uint8_t)cpu;
    tcb->migration_state = TASK_MIGRATION_STABLE;
    ready_list_check_prio(rq, prio, "add");
}

/**
 * @brief 从就绪队列中移除任务
 *
 * @param tcb 任务控制块指针
 *
 * @note 调用者必须持有临界区锁
 * @note 如果移除后链表为空，会清除位图中对应的位
 */
static int ready_list_remove_from_prio_internal(runqueue_t *rq, tcb_t *tcb,
                                                 uint8_t prio) {
    if (prio >= KERNEL_MAX_PRIORITIES) {
        return 0;
    }

    ready_list_t *list = &rq->ready_list[prio];
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
        bitmap_clear(rq, prio);
    }

    if (rq->ready_count > 0U) {
        rq->ready_count--;
    }
    ready_list_check_prio(rq, prio, "remove");
    return 1;
}

static int ready_list_contains_any_internal(const runqueue_t *rq, tcb_t *tcb) {
    if (tcb == NULL) {
        return 0;
    }

    for (uint8_t prio = 0; prio < KERNEL_MAX_PRIORITIES; prio++) {
        const ready_list_t *list = &rq->ready_list[prio];
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

static void ready_list_remove_internal(runqueue_t *rq, tcb_t *tcb) {
    uint8_t prio = tcb->priority;

    if (ready_list_remove_from_prio_internal(rq, tcb, prio)) {
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
        if (ready_list_remove_from_prio_internal(rq, tcb, p)) {
            return;
        }
    }
}

/**
 * @brief 获取指定优先级就绪队列的头节点
 * @param prio 优先级值
 * @return 队列头节点，如果队列为空则返回 NULL
 */
static tcb_t *ready_list_get_head(const runqueue_t *rq, uint8_t prio) {
    (void)prio;  /* 参数用于调试，避免未使用警告 */
    return rq->ready_list[prio].head;
}

static tcb_t *runq_get_highest(runqueue_t *rq) {
    int highest_prio = find_highest_prio(rq);

    while (highest_prio >= 0) {
        tcb_t *tcb = ready_list_get_head(rq, (uint8_t)highest_prio);
        if (tcb != NULL && tcb->name[0] != '\0') {
            return tcb;
        }

        /* A reclaimed TCB must never survive in a queue.  Drop the damaged
         * list defensively so release builds do not dispatch a zeroed stack. */
        rq->ready_list[highest_prio].head = NULL;
        rq->ready_list[highest_prio].tail = NULL;
        bitmap_clear(rq, (uint8_t)highest_prio);
        highest_prio = find_highest_prio(rq);
    }
    return NULL;
}

#if SMP
static uint32_t sched_choose_cpu(const tcb_t *tcb) {
    uint32_t current_cpu = sched_cpu();
    uint32_t allowed = tcb->affinity_mask & scheduler.online_mask;

    if (allowed == 0U) {
        allowed = (1UL << current_cpu) & scheduler.online_mask;
    }
    if (allowed == 0U) {
        allowed = 1U; /* core0 is the bootstrap CPU */
    }

    /* A blocked/suspended task keeps cache and wait-object locality unless
     * affinity or CPU online state forces a move. */
    if (tcb->cpu_owner < SMP_MAX_CPUS &&
        (allowed & (1UL << tcb->cpu_owner)) != 0U &&
        tcb->state != TASK_STATE_CREATED) {
        return tcb->cpu_owner;
    }

    uint32_t best_cpu = current_cpu;
    uint32_t best_load = UINT32_MAX;
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        if ((allowed & (1UL << cpu)) == 0U) {
            continue;
        }
        uint32_t load = runq_for(cpu)->ready_count;
        tcb_t *running = _current_task[cpu];
        if (running != NULL && running->id >= 0) {
            load++;
        }
        if (load < best_load || (load == best_load && cpu == current_cpu)) {
            best_load = load;
            best_cpu = cpu;
        }
    }
    return best_cpu;
}

static void remote_queue_push(uint32_t target, uint8_t op, tcb_t *tcb,
                              volatile uint32_t *completion) {
    remote_queue_t *queue = &scheduler.remote[target];

    for (;;) {
        uint32_t crit = irq_spin_lock(&queue->lock);
        if (queue->count < SCHED_REMOTE_QUEUE_LEN) {
            remote_op_t *entry = &queue->entries[queue->tail];
            entry->tcb = tcb;
            entry->completion = completion;
            entry->op = op;
            queue->tail = (uint16_t)((queue->tail + 1U) %
                                     SCHED_REMOTE_QUEUE_LEN);
            queue->count++;
            irq_spin_unlock(&queue->lock, crit);
            break;
        }
        irq_spin_unlock(&queue->lock, crit);
        smp_send_ipi(target, SMP_IPI_REMOTE_QUEUE);
        __asm volatile("yield");
    }

    smp_send_ipi(target, SMP_IPI_REMOTE_QUEUE);

    if (completion != NULL) {
        while (__atomic_load_n(completion, __ATOMIC_ACQUIRE) == 0U) {
            __asm volatile("yield");
        }
    }
}

static void sched_drain_remote_queue(void) {
    uint32_t cpu = sched_cpu();
    runqueue_t *rq = runq_for(cpu);
    remote_queue_t *queue = &scheduler.remote[cpu];
    int added = 0;

    for (;;) {
        remote_op_t op;
        uint32_t crit = irq_spin_lock(&queue->lock);
        if (queue->count == 0U) {
            irq_spin_unlock(&queue->lock, crit);
            break;
        }
        op = queue->entries[queue->head];
        queue->head = (uint16_t)((queue->head + 1U) %
                                 SCHED_REMOTE_QUEUE_LEN);
        queue->count--;
        irq_spin_unlock(&queue->lock, crit);

        if (op.tcb != NULL) {
            switch ((remote_op_type_t)op.op) {
            case REMOTE_OP_ADD:
                if (op.tcb->state == TASK_STATE_READY &&
                    !ready_list_contains_any_internal(rq, op.tcb)) {
                    ready_list_add_internal(rq, cpu, op.tcb);
                    added = 1;
                }
                break;
            case REMOTE_OP_REMOVE:
                if (ready_list_contains_any_internal(rq, op.tcb)) {
                    ready_list_remove_internal(rq, op.tcb);
                }
                break;
            case REMOTE_OP_REINSERT:
                if (ready_list_contains_any_internal(rq, op.tcb)) {
                    ready_list_remove_internal(rq, op.tcb);
                    ready_list_add_internal(rq, cpu, op.tcb);
                    added = 1;
                }
                break;
            case REMOTE_OP_QUIESCE:
                if (_current_task[cpu] == op.tcb) {
                    if (op.tcb->state == TASK_STATE_RUNNING ||
                        op.tcb->state == TASK_STATE_READY) {
                        op.tcb->state = TASK_STATE_SUSPENDED;
                    }
                    scheduler.quiesce_completion[cpu] = op.completion;
                    rq->need_resched = 1;
                    op.completion = NULL;
                    hal_trigger_pendsv();
                } else {
                    if (ready_list_contains_any_internal(rq, op.tcb)) {
                        ready_list_remove_internal(rq, op.tcb);
                    }
                    if (op.tcb->state == TASK_STATE_READY ||
                        op.tcb->state == TASK_STATE_RUNNING) {
                        op.tcb->state = TASK_STATE_SUSPENDED;
                    }
                }
                break;
            default:
                break;
            }
        }

        if (op.completion != NULL) {
            __atomic_store_n(op.completion, 1U, __ATOMIC_RELEASE);
        }
    }

    if (added) {
        scheduler.steal_pending[cpu] = 0U;
        rq->need_resched = 1;
        hal_trigger_pendsv();
    }
}

static void sched_steal_one(uint32_t requester) {
    uint32_t cpu = sched_cpu();
    runqueue_t *rq = runq_for(cpu);
    tcb_t *victim = NULL;

    if (requester >= SMP_MAX_CPUS || requester == cpu) {
        return;
    }

    /* Donate the lowest-priority movable READY task.  The donor CPU is the
     * only writer of this run queue, so no global scheduler lock is needed. */
    for (int prio = KERNEL_MAX_PRIORITIES - 1; prio >= 0; prio--) {
        for (tcb_t *it = rq->ready_list[prio].tail; it != NULL; it = it->prev) {
            if ((it->affinity_mask & (1UL << requester)) != 0U &&
                it->migration_state == TASK_MIGRATION_STABLE) {
                victim = it;
                break;
            }
        }
        if (victim != NULL) {
            break;
        }
    }

    if (victim == NULL) {
        smp_send_ipi(requester, SMP_IPI_STEAL_COMPLETE);
        return;
    }

    victim->migration_state = TASK_MIGRATION_MIGRATING;
    ready_list_remove_internal(rq, victim);
    victim->cpu_owner = (uint8_t)requester;
    victim->migration_state = TASK_MIGRATION_READY_REMOTE;
    remote_queue_push(requester, REMOTE_OP_ADD, victim, NULL);
}

static void sched_request_steal(void) {
    uint32_t cpu = sched_cpu();
    uint32_t peers = scheduler.online_mask & ~(1UL << cpu);

    /* Core 1 reaches its idle PendSV before core 0 has committed the initial
     * task selection.  Stealing during that bootstrap window can run the test
     * runner before sched_start() has published a valid core-0 current task. */
    if (__atomic_load_n(&scheduler.started, __ATOMIC_ACQUIRE) == 0 ||
        peers == 0U || scheduler.steal_pending[cpu] != 0U) {
        return;
    }
    uint32_t donor = (uint32_t)__builtin_ctz(peers);
    scheduler.steal_pending[cpu] = 1U;
    smp_send_ipi(donor, SMP_IPI_STEAL_REQUEST);
}

void sched_handle_ipi(uint32_t reasons) {
    uint32_t cpu = sched_cpu();

    if ((reasons & SMP_IPI_REMOTE_QUEUE) != 0U) {
        sched_drain_remote_queue();
    }
    if ((reasons & SMP_IPI_STEAL_REQUEST) != 0U) {
        sched_steal_one(cpu ^ 1U);
    }
    if ((reasons & SMP_IPI_STEAL_COMPLETE) != 0U) {
        scheduler.steal_pending[cpu] = 0U;
    }
    if ((reasons & SMP_IPI_RESCHEDULE) != 0U) {
        runq_for(cpu)->need_resched = 1;
        hal_trigger_pendsv();
    }
}

void sched_set_cpu_online(uint32_t cpu, int online) {
    if (cpu >= SMP_MAX_CPUS) {
        return;
    }
    if (online) {
        __atomic_fetch_or(&scheduler.online_mask, (1UL << cpu),
                          __ATOMIC_RELEASE);
    } else {
        __atomic_fetch_and(&scheduler.online_mask, ~(1UL << cpu),
                           __ATOMIC_RELEASE);
    }
}
#endif

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
    irq_spin_init_rank(&sched_lock, LOCKDEP_RANK_REGISTRY);

    uint32_t crit = irq_spin_lock(&sched_lock);

    memset((void *)&scheduler, 0, sizeof(scheduler));
    timeout_service_tcb = NULL;
    scheduler.online_mask = 1U;
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        _current_task[cpu] = NULL;
        _next_task[cpu] = NULL;
#if SMP
        irq_spin_init_rank(&scheduler.remote[cpu].lock, LOCKDEP_RANK_REMOTE);
#endif
    }

    irq_spin_unlock(&sched_lock, crit);
}

/*============================================================================
 * Deferred timeout/reclaim service
 *============================================================================*/

static void sched_process_timeouts(void) {
    uint32_t now = sched_get_tick_count();
    uint64_t used_snapshot = task_get_used_bitmap();

    for (task_id_t id = 0; id < KERNEL_MAX_TASKS; id++) {
        if ((used_snapshot & (1ULL << id)) == 0U) continue;

        tcb_t *tcb = task_get_tcb(id);
        if (tcb == NULL || tcb->state != TASK_STATE_BLOCKED ||
            tcb->cont.deadline == 0U || tcb->cont.deadline > now) {
            continue;
        }

        kern_err_t wake_result =
            (tcb->cont.op == BLOCK_REASON_SLEEP) ? KERN_OK :
                                                        KERN_ERR_TIMEOUT;
        if (tcb->cont.op == BLOCK_REASON_JOIN) {
            task_cancel_join_wait(tcb);
        } else if (tcb->cont.active) {
            (void)task_cancel_blocked_wait(tcb);
        }
        sched_wakeup(tcb, wake_result);
    }

    task_reclaim_expired();
#if KERN_TASK_STATS
    stats_deferred_update();
#endif
}

static void sched_timeout_service_task(void *arg) {
    (void)arg;

    for (;;) {
#if CAP_ENABLE
        /* Fallback safe point for producers wrapped by a raw
         * hal_irq_save/restore region rather than an irq_spinlock.  Normal
         * capability calls drain synchronously at their outermost unlock; the
         * core0 service guarantees an unusual pending item cannot be stranded. */
        cap_deferred_poll();
#endif
        sched_process_timeouts();
        (void)sched_block(BLOCK_REASON_TIMER, &scheduler, KERN_WAIT_FOREVER);
    }
}

void sched_timeout_service_start(void) {
    task_id_t id = task_create("timeout_svc", sched_timeout_service_task,
                               NULL, 0U, 512U);
    if (id < 0) {
#if KERN_DEBUG_ENABLE
        kern_panic("timeout service create failed");
#endif
        return;
    }

    (void)task_set_affinity(id, 1UL);
    timeout_service_tcb = task_get_tcb(id);
    (void)task_start(id);
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
    uint32_t cpu = sched_cpu();
    runqueue_t *rq = runq_for(cpu);

    /* 检查是否有任务可运行 */
    int has_task = 0;
    for (int i = 0; i < 4; i++) {
        if (rq->ready_bitmap[i] != 0) {
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
    tcb_t *first = runq_get_highest(rq);
    if (first == NULL) {
        irq_spin_unlock(&sched_lock, crit);
        hal_debug_puts("[SCHED] get_highest_ready returned NULL!\r\n");
        while (1) {
            hal_enter_lowpower();
        }
    }

    /* 从就绪队列移除（即将变为 RUNNING 状态） */
    ready_list_remove_internal(rq, first);

    /* 设置为运行状态 */
    first->state = TASK_STATE_RUNNING;

    /*
     * 关键：设置 _current_task = NULL
     * SVC 处理程序会检查这个值，如果是 NULL 则跳过上下文保存
     * 这是首次切换的特殊情况
     */
    first->cpu_owner = (uint8_t)cpu;
    first->migration_state = TASK_MIGRATION_STABLE;
    _current_task[cpu] = NULL;
    _next_task[cpu] = first;
    /* 不再设 scheduler.current_task (per-CPU 用 _current_task) */

    __atomic_store_n(&scheduler.started, 1, __ATOMIC_RELEASE);

    irq_spin_unlock(&sched_lock, crit);

#if SMP
    /* Core 1 may already be running its idle task after being held behind the
     * bootstrap steal gate.  Kick it now so it can request work immediately. */
    smp_send_ipi(1U, SMP_IPI_RESCHEDULE);
#endif

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
    runq_for(sched_cpu())->need_resched = 1;
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
    if (tcb == NULL) {
        return;
    }

    uint32_t cpu = sched_cpu();
#if SMP
    uint32_t target = sched_choose_cpu(tcb);
#endif
    tcb->state = TASK_STATE_READY;

#if SMP
    if (target != cpu) {
        tcb->cpu_owner = (uint8_t)target;
        tcb->migration_state = TASK_MIGRATION_READY_REMOTE;
        remote_queue_push(target, REMOTE_OP_ADD, tcb, NULL);
        return;
    }
#endif

    runqueue_t *rq = runq_for(cpu);
    uint32_t crit = hal_irq_save();

    /* 已经在就绪队列中则不重复添加 */
    if (ready_list_contains_any_internal(rq, tcb)) {
        hal_irq_restore(crit);
        return;
    }

    /* 更新状态并加入就绪队列 */
    ready_list_add_internal(rq, cpu, tcb);

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

    hal_irq_restore(crit);

    if (need_preempt) {
        rq->need_resched = 1;
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
    if (tcb == NULL || tcb->cpu_owner >= SMP_MAX_CPUS) {
        return;
    }

    uint32_t cpu = sched_cpu();
#if SMP
    if (tcb->cpu_owner != cpu) {
        volatile uint32_t completion = 0U;
        remote_queue_push(tcb->cpu_owner, REMOTE_OP_REMOVE, tcb,
                          &completion);
        return;
    }
#endif

    runqueue_t *rq = runq_for(cpu);
    uint32_t crit = hal_irq_save();

    if (!ready_list_contains_any_internal(rq, tcb)) {
        hal_irq_restore(crit);
        return;
    }

    ready_list_remove_internal(rq, tcb);
    hal_irq_restore(crit);
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
    if (tcb == NULL || tcb->cpu_owner >= SMP_MAX_CPUS) {
        return;
    }

    uint32_t cpu = sched_cpu();
#if SMP
    if (tcb->cpu_owner != cpu) {
        volatile uint32_t completion = 0U;
        remote_queue_push(tcb->cpu_owner, REMOTE_OP_REINSERT, tcb,
                          &completion);
        return;
    }
#endif

    runqueue_t *rq = runq_for(cpu);
    uint32_t crit = hal_irq_save();
    if (!ready_list_contains_any_internal(rq, tcb)) {
        hal_irq_restore(crit);
        return;
    }

    /* 先移除再重新插入 */
    ready_list_remove_internal(rq, tcb);
    ready_list_add_internal(rq, cpu, tcb);
    hal_irq_restore(crit);
}

kern_err_t sched_quiesce_task(tcb_t *tcb) {
    if (tcb == NULL || tcb->cpu_owner >= SMP_MAX_CPUS) {
        return KERN_ERR_PARAM;
    }

    uint32_t cpu = sched_cpu();
    if (tcb->cpu_owner == cpu) {
        if (tcb == _current_task[cpu]) {
            return KERN_ERR_BUSY;
        }
        sched_remove_ready(tcb);
        if (tcb->state == TASK_STATE_READY) {
            tcb->state = TASK_STATE_SUSPENDED;
        }
        return KERN_OK;
    }

#if SMP
    volatile uint32_t completion = 0U;
    remote_queue_push(tcb->cpu_owner, REMOTE_OP_QUIESCE, tcb, &completion);
    return KERN_OK;
#else
    return KERN_ERR_STATE;
#endif
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

    uint32_t cpu = sched_cpu();
    runqueue_t *rq = runq_for(cpu);
    uint32_t crit = hal_irq_save();

    /* 从就绪队列移除 */
    if (current->state == TASK_STATE_READY) {
        ready_list_remove_internal(rq, current);
    }

    /* 设置阻塞状态 */
    current->state = TASK_STATE_BLOCKED;
    current->cont.op = reason;
    current->cont.object = obj;
    current->cont.result = KERN_OK;

    /* 设置超时唤醒时间；0/WAIT_FOREVER 均表示无期限等待。 */
    current->cont.deadline = sched_timeout_deadline(timeout);

    rq->need_resched = 1;

    /*
     * 解锁并触发 PendSV
     * 注意顺序：先解锁，再触发 PendSV
     * 这样 PendSV 可以在临界区外执行
     */
    hal_irq_restore(crit);
    hal_trigger_pendsv();

    /*
     * 当任务被唤醒后，会从这里继续执行
     * block_result 已经由 sched_wakeup() 设置
     */
    return current->cont.result;
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
    if (tcb == NULL) {
        return;
    }

    /* Local IRQ masking is not a cross-core exclusion mechanism.  Timeout,
     * delete and IPC delivery may race on different CPUs; exactly one of
     * them is allowed to claim BLOCKED -> READY.  Without this CAS the same
     * TCB can be queued and run on both CPUs, corrupting its PSP frame. */
    task_state_t expected = TASK_STATE_BLOCKED;
    if (!__atomic_compare_exchange_n(&tcb->state, &expected,
                                     TASK_STATE_READY, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return;
    }

    uint32_t crit = hal_irq_save();

    /* 清除阻塞信息 (block_obj 保留，由 IPC 原语负责清理等待队列) */
    tcb->cont.op = BLOCK_REASON_NONE;
    tcb->cont.result = result;
    tcb->cont.deadline = 0;
    task_complete_blocked_syscall(tcb, result);

    hal_irq_restore(crit);

    /* Placement and remote IPI delivery are centralized here. */
    sched_add_ready(tcb);
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
    /* HAL implementations used by early boot and host-style tests may
     * transiently report an out-of-range CPU id.  Never index the per-CPU
     * table with that value: doing so can corrupt adjacent scheduler state
     * precisely while an SMP wakeup is being delivered. */
    uint32_t cpu = hal_get_cpu_id();
    if (cpu >= SMP_MAX_CPUS) {
        cpu = 0U;
    }
    return _current_task[cpu];
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
    return runq_get_highest(runq_for(sched_cpu()));
}

/**
 * @brief 检查是否需要调度
 * @return 1 表示需要调度，0 表示不需要
 */
int sched_need_switch(void) {
    return runq_for(sched_cpu())->need_resched;
}

/**
 * @brief 获取系统滴答计数
 * @return 当前滴答计数
 */
uint32_t sched_get_tick_count(void) {
    return scheduler.tick_count;
}

uint32_t sched_timeout_deadline(uint32_t timeout) {
    if (timeout == 0U || timeout == KERN_WAIT_FOREVER) {
        return 0U;
    }

    return sched_get_tick_count() + timeout;
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
    /* Core 0 is the sole wall-clock timekeeper.  Each CPU has its own SysTick,
     * so incrementing here on both cores makes timeouts and timers run at
     * roughly twice real time.  Core 1 still performs its local time-slice
     * accounting below, but must never advance the shared clock. */
#if SMP
    uint32_t cpu = hal_get_cpu_id();
    if (cpu == 0U) {
        scheduler.tick_count++;
    }
#else
    uint32_t cpu = 0U;
    scheduler.tick_count++;
#endif

    runqueue_t *rq = runq_for(cpu);

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
                rq->need_resched = 1;
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
            if (rq->ready_bitmap[i] != 0) {
                has_ready = 1;
                break;
            }
        }
        if (has_ready) {
            rq->need_resched = 1;
            hal_trigger_pendsv();
        }
    }

#if KERN_TASK_STATS
    /* Per-CPU accounting is lock-free. CPU-usage aggregation is deferred to
     * timeout_svc because SysTick must never spin on task_lock. */
    stats_tick_update();
#endif

    /*
     * 超时唤醒检查 (仅 core0 执行,避免两核重复唤醒)
     * SMP 下两核 SysTick 同时触发 tick handler,如果都做超时唤醒,
     * timer_svc/notification 的时序会被打乱 (timer callback 在 timer_svc
     * 上下文执行,两核同时 process_expired_timers 导致竞态)。
     * 只让 core0 做超时唤醒 + timer_svc 处理,core1 只做时间片/idle。
     */
#if SMP
    if (cpu != 0U) {
        return;  /* core1 不做超时唤醒 */
    }
#endif

    /* Wake only the core0-pinned worker.  This path mutates the local runqueue
     * with IRQs already masked and never waits for a cross-core lock. */
    if (timeout_service_tcb != NULL &&
        timeout_service_tcb->state == TASK_STATE_BLOCKED &&
        timeout_service_tcb->cont.op == BLOCK_REASON_TIMER &&
        timeout_service_tcb->cont.object == &scheduler) {
        sched_wakeup(timeout_service_tcb, KERN_OK);
    }

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
    uint32_t cpu = sched_cpu();
    runqueue_t *rq = runq_for(cpu);
    rq->need_resched = 0;

    tcb_t *current = _current_task[cpu];

    /* 内存屏障：确保读取正确的值 */
    __asm volatile("dmb");

    /* PendSV runs with local interrupts masked and is the sole mutator of
     * this CPU's run queue.  Remote CPUs can only submit IPI commands, so no
     * shared scheduler spinlock is present on the context-switch path. */

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
                ready_list_add_internal(rq, cpu, current);
                break;

            case TASK_STATE_TERMINATED:
                /* Termination publishes reclaim_at before PendSV.  Never take
                 * the global task-pool lock from the switch path. */
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
    tcb_t *next = runq_get_highest(rq);

    if (next == NULL || next->name[0] == '\0') {
        next = task_get_idle();
#if SMP
        sched_request_steal();
#endif
    } else {
        ready_list_remove_internal(rq, next);
    }

    next->state = TASK_STATE_RUNNING;

    next->cpu_owner = (uint8_t)cpu;
    next->migration_state = TASK_MIGRATION_STABLE;

    /* 更新 per-CPU 指针 (供汇编加载上下文) */

#if TRACE_ENABLE
    trace_record(TRACE_TASK_SWITCH, (uint8_t)(next->id >= 0 ? next->id : 0), 0);
#endif

    /* 内存屏障：确保写入完成 */
    __asm volatile("dmb");

    /* 更新全局指针，供汇编代码使用 (per-CPU indexed) */
    _current_task[cpu] = next;
    _next_task[cpu] = next;

#if KERN_TASK_STATS
    stats_task_switch(current, next);
#endif

#if SMP
    /* A remote deleter/suspender may release the old TCB only after every C
     * path in this PendSV has stopped referencing it. */
    volatile uint32_t *completion = scheduler.quiesce_completion[cpu];
    if (completion != NULL) {
        scheduler.quiesce_completion[cpu] = NULL;
        __atomic_store_n(completion, 1U, __ATOMIC_RELEASE);
    }
#endif
}
