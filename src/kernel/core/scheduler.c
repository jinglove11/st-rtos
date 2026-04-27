/**
 * @file scheduler.c
 * @brief 调度器实现
 */

#include "scheduler.h"
#include "kernel_config.h"
#include "kernel_types.h"
#include "task.h"
#include "hal.h"

extern tcb_t *task_get_tcb(task_id_t task_id);
extern task_id_t task_get_next(task_id_t task_id);

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
    volatile int need_schedule;
    int started;

#if KERN_TASK_STATS
    uint32_t last_stat_tick;
#endif

} scheduler;

/*============================================================================
 * 临界区保护
 *============================================================================*/

// 进入临界区 (关中断)
static inline uint32_t crit_enter(void) {
    uint32_t primask = hal_irq_save();
    return primask;
}

// 退出临界区 (恢复中断)
static inline void crit_exit(uint32_t primask) {
    hal_irq_restore(primask);
}

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
    uint32_t crit = crit_enter();

    for (int i = 0; i < KERN_MAX_PRIORITY; i++) {
        scheduler.ready_list[i].head = NULL;
        scheduler.ready_list[i].tail = NULL;
    }
    scheduler.current_task = NULL;
    for (int i = 0; i < 4; i++) {
        scheduler.ready_bitmap[i] = 0;
    }
    scheduler.tick_count = 0;
    scheduler.need_schedule = 0;
    scheduler.started = 0;

    crit_exit(crit);
}

void sched_start(void) {
    uint32_t crit = crit_enter();

    int has_task = 0;
    for (int i = 0; i < 4; i++) {
        if (scheduler.ready_bitmap[i] != 0) {
            has_task = 1;
            break;
        }
    }
    if (!has_task) {
        crit_exit(crit);
        while (1) {
            hal_enter_lowpower();
        }
    }

    tcb_t *first = sched_get_highest_ready();
    if (first == NULL) {
        crit_exit(crit);
        while (1) {
            hal_enter_lowpower();
        }
    }

    ready_list_remove_internal(first);

    first->state = TASK_STATE_RUNNING;

    _current_task = NULL;
    _next_task = first;
    scheduler.current_task = first;

    scheduler.started = 1;

    hal_systick_init(KERN_TICK_RATE_HZ);

    hal_irq_enable();

    hal_trigger_first_switch();

    while (1);
}

void sched_yield(void) {
    scheduler.need_schedule = 1;
    hal_trigger_pendsv();
}

void sched_add_ready(tcb_t *tcb) {
    uint32_t crit = crit_enter();

    if (tcb->state == TASK_STATE_READY) {
        crit_exit(crit);
        return;  // 已经在就绪队列
    }

    tcb->state = TASK_STATE_READY;
    ready_list_add_internal(tcb);

    // 如果调度器已启动且优先级高于当前任务, 触发调度
    if (scheduler.started &&
        scheduler.current_task &&
        tcb->priority < scheduler.current_task->priority) {
        scheduler.need_schedule = 1;
        hal_trigger_pendsv();
    }

    crit_exit(crit);
}

void sched_remove_ready(tcb_t *tcb) {
    uint32_t crit = crit_enter();

    if (tcb->state != TASK_STATE_READY) {
        crit_exit(crit);
        return;
    }

    ready_list_remove_internal(tcb);
    crit_exit(crit);
}

kern_err_t sched_block(block_reason_t reason, void *obj, uint32_t timeout) {
    tcb_t *current = scheduler.current_task;

    if (current == NULL) {
        return KERN_ERR_STATE;
    }

    // 设置阻塞状态
    current->state = TASK_STATE_BLOCKED;
    current->block_reason = reason;
    current->block_obj = obj;
    current->block_result = KERN_OK;

    if (timeout > 0) {
        current->wake_tick = scheduler.tick_count + timeout;
    } else {
        current->wake_tick = 0;
    }

    scheduler.need_schedule = 1;

    // 使用 SVC 进行阻塞式上下文切换
    // SVC 会立即触发异常，保存当前上下文，切换到下一个任务
    // 当任务被唤醒后再次被调度时，会从这里返回
    hal_trigger_block_switch();

    // 当任务再次运行时，返回阻塞结果
    return current->block_result;
}

void sched_wakeup(tcb_t *tcb, kern_err_t result) {
    uint32_t crit = crit_enter();

    if (tcb->state != TASK_STATE_BLOCKED) {
        crit_exit(crit);
        return;
    }

    tcb->block_reason = BLOCK_REASON_NONE;
    tcb->block_obj = NULL;
    tcb->block_result = result;
    tcb->wake_tick = 0;

    tcb->state = TASK_STATE_READY;
    ready_list_add_internal(tcb);

    scheduler.need_schedule = 1;
    hal_trigger_pendsv();

    crit_exit(crit);
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
    return scheduler.need_schedule;
}

void sched_tick_handler(void) {
    scheduler.tick_count++;

    tcb_t *current = scheduler.current_task;
    if (current == NULL) {
        return;
    }

#if KERN_TIME_SLICE
    if (current->time_slice > 0) {
        current->time_slice--;

        if (current->time_slice == 0) {
            scheduler.need_schedule = 1;
            hal_trigger_pendsv();
        }
    }
#endif

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
    scheduler.need_schedule = 0;

    tcb_t *current = _current_task;

    if (current && current->state == TASK_STATE_RUNNING) {
        current->time_slice = current->time_slice_reload;
        current->state = TASK_STATE_READY;
        // 空闲任务也需要加入就绪队列，确保始终有任务可调度
        ready_list_add_internal(current);
    }

    tcb_t *next = sched_get_highest_ready();

    if (next == NULL) {
        next = task_get_idle();
    } else {
        ready_list_remove_internal(next);
    }

    next->state = TASK_STATE_RUNNING;
    scheduler.current_task = next;
    _current_task = next;
    _next_task = next;

#if KERN_TASK_STATS
    if (current) {
        current->ctx_switch_count++;
    }
    next->ctx_switch_count++;
#endif
}
