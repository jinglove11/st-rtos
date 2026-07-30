/**
 * @file event.c
 * @brief 事件标志组实现
 */

#include "event.h"
#include "wait_queue.h"
#include "scheduler.h"
#include "kernel_config.h"
#include "hal.h"
#include "spinlock.h"
#include "syscall.h"
#include "continuation.h"
#include "capability.h"
#include <string.h>

/*============================================================================
 * 事件等待控制块 (存储在任务的 block_obj 中)
 *============================================================================*/

/*============================================================================
 * 静态分配的事件标志组池
 *============================================================================*/

static event_t event_pool[KERN_MAX_EVENTS];
static uint32_t event_used_bitmap;
static irq_spinlock_t event_lock; /* M1: SMP safe */

/*============================================================================
 * 内部函数
 *============================================================================*/

// 分配事件标志组 ID
static event_id_t alloc_event_id(void) {
    for (int i = 0; i < KERN_MAX_EVENTS; i++) {
        if (!(event_used_bitmap & (1U << i)) &&
            !kobj_generation_is_retired(event_pool[i].hdr.generation)) {
            event_used_bitmap |= (1U << i);
            return (event_id_t)i;
        }
    }
    return KERN_INVALID_ID;
}

// 释放事件标志组 ID
static void free_event_id(event_id_t id) {
    if (id >= 0 && id < KERN_MAX_EVENTS) {
        event_used_bitmap &= ~(1U << id);
    }
}

// 获取事件标志组指针
static event_t *get_event(event_id_t id) {
    if (id < 0 || id >= KERN_MAX_EVENTS) {
        return NULL;
    }
    if (!event_pool[id].in_use) {
        return NULL;
    }
    return &event_pool[id];
}

// 检查事件是否满足
static int event_check(uint32_t current, uint32_t wait, uint32_t opt) {
    if (opt & EVENT_OPT_OR) {
        // OR: 任一标志设置
        return (current & wait) != 0;
    } else {
        // AND: 所有标志都设置
        return (current & wait) == wait;
    }
}

/*============================================================================
 * 公开接口实现
 *============================================================================*/

void event_init(void) {
    irq_spin_init_rank(&event_lock, LOCKDEP_RANK_OBJECT);
    memset(event_pool, 0, sizeof(event_pool));
    event_used_bitmap = 0;
}

event_id_t event_create(uint32_t initial_flags) {
    uint32_t crit = irq_spin_lock(&event_lock);

    event_id_t id = alloc_event_id();
    if (id == KERN_INVALID_ID) {
        irq_spin_unlock(&event_lock, crit);
        return KERN_INVALID_ID;
    }

    event_t *evt = &event_pool[id];
    /* M2-Step3a: 初始化对象 header */
    if (evt->hdr.generation == 0) {
        kobj_header_init(&evt->hdr, CAP_OBJ_EVENT);
    } else {
        evt->hdr.obj_type = CAP_OBJ_EVENT;
    }
    evt->flags = initial_flags;
    evt->in_use = 1;
    wait_queue_init(&evt->wait_queue);

    irq_spin_unlock(&event_lock, crit);
    return id;
}

kern_err_t event_delete(event_id_t event_id) {
    uint32_t crit = irq_spin_lock(&event_lock);

    event_t *evt = get_event(event_id);
    if (evt == NULL) {
        irq_spin_unlock(&event_lock, crit);
        return KERN_ERR_PARAM;
    }

    // 唤醒所有等待的任务
    tcb_t *tcb = evt->wait_queue.head;
    while (tcb) {
        tcb_t *next = tcb->wait_next;
        tcb->wait_next = NULL;
        tcb->wait_prev = NULL;
        memset(&tcb->cont.u.event, 0, sizeof(tcb->cont.u.event));
        tcb->cont.result = KERN_ERR_NOEXIST;
        sched_wakeup(tcb, KERN_ERR_NOEXIST);
        tcb = next;
    }

    // 清零并释放
#if CAP_ENABLE
    /* M2-Step1+3a: 撤销所有任务持有的指向此 event 的 cap。Step3a 改真指针。 */
    (void)cap_revoke_object(evt, CAP_OBJ_EVENT);
#endif
    /* M2-Step3a: bump generation 跨 memset 保留 */
    uint32_t next_gen = kobj_header_prepare_reuse(&evt->hdr);
    memset(evt, 0, sizeof(event_t));
    evt->hdr.obj_type   = CAP_OBJ_EVENT;
    evt->hdr.generation = next_gen;
    free_event_id(event_id);

    irq_spin_unlock(&event_lock, crit);
    return KERN_OK;
}

/* M2-Step3a: cap 路径 id ↔ 对象指针 转换。 */
event_id_t event_id_from_obj(void *obj) {
    if (obj == NULL) return KERN_INVALID_ID;
    event_t *evt = (event_t *)obj;
    event_id_t id = (event_id_t)(evt - event_pool);
    if (id < 0 || id >= KERN_MAX_EVENTS) return KERN_INVALID_ID;
    return id;
}

void *event_obj_for_cap(event_id_t id) {
    if (id < 0 || id >= KERN_MAX_EVENTS) return NULL;
    return (void *)&event_pool[id];
}

void event_cleanup_task(void *event_obj, tcb_t *tcb) {
    event_t *evt = (event_t *)event_obj;
    if (evt == NULL || tcb == NULL) return;

    uint32_t crit = irq_spin_lock(&event_lock);
    if (evt >= &event_pool[0] && evt < &event_pool[KERN_MAX_EVENTS] &&
        evt->in_use) {
        (void)wait_queue_remove_safe(&evt->wait_queue, tcb);
    }
    irq_spin_unlock(&event_lock, crit);
}

kern_err_t event_wait(event_id_t event_id, uint32_t flags, uint32_t opt,
                      uint32_t timeout, uint32_t *received) {
    if (hal_irq_get_active() >= 0) {
        return KERN_ERR_ISR;
    }
    uint32_t crit = irq_spin_lock(&event_lock);

    event_t *evt = get_event(event_id);
    if (evt == NULL) {
        irq_spin_unlock(&event_lock, crit);
        return KERN_ERR_PARAM;
    }

    if (event_check(evt->flags, flags, opt)) {
        if (received) {
            *received = evt->flags;
        }
        if (opt & EVENT_OPT_CLEAR) {
            evt->flags &= ~flags;
        }
        irq_spin_unlock(&event_lock, crit);
        return KERN_OK;
    }

    /* NOWAIT (poll): return the current word immediately without blocking,
     * regardless of whether `flags` matched. Clear only the requested bits if
     * CLEAR is also requested. This is the seL4 "poll" equivalent. */
    if (opt & EVENT_OPT_NOWAIT) {
        if (received) {
            *received = evt->flags;
        }
        if (opt & EVENT_OPT_CLEAR) {
            evt->flags &= ~flags;
        }
        irq_spin_unlock(&event_lock, crit);
        return KERN_OK;
    }

    if (timeout == 0) {
        irq_spin_unlock(&event_lock, crit);
        return KERN_ERR_TIMEOUT;
    }

    tcb_t *current = sched_get_current();
    current->cont.op = BLOCK_REASON_EVENT;
    current->cont.object = evt;

    /* M3-Task3: continuation 存 TCB 而非全局 side table */
    current->cont.u.event.wait_flags = flags;
    current->cont.u.event.wait_opt = opt;
    current->cont.u.event.received = received;

    wait_queue_add(&evt->wait_queue, current);

    /* 从就绪队列移除 */
    {
        extern void sched_remove_ready(tcb_t *tcb);
        sched_remove_ready(current);
    }

    /* 设置阻塞状态 */
    current->state = TASK_STATE_BLOCKED;
    current->cont.result = KERN_OK;

    current->cont.deadline = sched_timeout_deadline(timeout);

    irq_spin_unlock(&event_lock, crit);

    /* 触发上下文切换 */
    hal_trigger_pendsv();

    /* 等待被唤醒 */
    while (current->state == TASK_STATE_BLOCKED) {
        __asm volatile("wfi");
        __asm volatile("dmb");
    }

    kern_err_t result = current->cont.result;

    if (result == KERN_OK) {
        /* The signaler (event_set) already copied the matched word into
         * `*received` and applied CLEAR before waking us (see the wake loop in
         * event_set). Re-reading evt->flags here would yield stale/zeroed
         * bits (CLEAR already ran), so we must NOT overwrite `*received` or
         * re-CLEAR. The fast-path (non-blocking match above) handles its own
         * copy+clear. Only ensure the task is unlinked from the wait queue if
         * the signaler didn't (it normally does). */
        crit = irq_spin_lock(&event_lock);
        if (current->cont.object == evt) {
            wait_queue_remove(&evt->wait_queue, current);
            current->cont.object = NULL;
        }
        irq_spin_unlock(&event_lock, crit);
    } else {
        crit = irq_spin_lock(&event_lock);
        if (current->cont.object == evt) {
            wait_queue_remove(&evt->wait_queue, current);
            current->cont.object = NULL;
        }
        irq_spin_unlock(&event_lock, crit);
    }

    return result;
}

#if SYSCALL_ENABLE
kern_err_t event_wait_syscall(event_id_t event_id, uint32_t flags,
                              uint32_t opt, uint32_t timeout) {
    if (hal_irq_get_active() >= 0) {
        return KERN_ERR_ISR;
    }

    uint32_t crit = irq_spin_lock(&event_lock);

    event_t *evt = get_event(event_id);
    if (evt == NULL) {
        irq_spin_unlock(&event_lock, crit);
        return KERN_ERR_PARAM;
    }

    if (event_check(evt->flags, flags, opt)) {
        if (opt & EVENT_OPT_CLEAR) {
            evt->flags &= ~flags;
        }
        irq_spin_unlock(&event_lock, crit);
        return KERN_OK;
    }

    if (timeout == 0) {
        irq_spin_unlock(&event_lock, crit);
        return KERN_ERR_TIMEOUT;
    }

    tcb_t *current = sched_get_current();
    if (current == NULL ||
        current->id < 0 || current->id >= KERN_MAX_TASKS) {
        irq_spin_unlock(&event_lock, crit);
        return KERN_ERR_STATE;
    }

    current->cont.u.event.wait_flags = flags;
    current->cont.u.event.wait_opt = opt;
    current->cont.u.event.received = NULL;

    /* M3-Task3: 两阶段阻塞协议 */
    syscall_cont_prepare_locked(BLOCK_REASON_EVENT, evt);
    wait_queue_add(&evt->wait_queue, current);

    irq_spin_unlock(&event_lock, crit);
    return syscall_cont_commit(timeout);
}
#endif

kern_err_t event_set(event_id_t event_id, uint32_t flags) {
    uint32_t crit = irq_spin_lock(&event_lock);

    event_t *evt = get_event(event_id);
    if (evt == NULL) {
        irq_spin_unlock(&event_lock, crit);
        return KERN_ERR_PARAM;
    }

    evt->flags |= flags;

    tcb_t *tcb = evt->wait_queue.head;
    while (tcb) {
        tcb_t *next = tcb->wait_next;

        task_id_t tid = tcb->id;

        if (tid < 0 || tid >= KERN_MAX_TASKS) {
            tcb = next;
            continue;
        }

        /* M3-Task3: continuation 从 TCB 读 */
        uint32_t wait_flags = tcb->cont.u.event.wait_flags;
        uint32_t wait_opt = tcb->cont.u.event.wait_opt;

        if (event_check(evt->flags, wait_flags, wait_opt)) {
            if (tcb->cont.u.event.received != NULL) {
                *tcb->cont.u.event.received = evt->flags;
            }
            if (wait_opt & EVENT_OPT_CLEAR) {
                evt->flags &= ~wait_flags;
            }
            memset(&tcb->cont.u.event, 0, sizeof(tcb->cont.u.event));
            wait_queue_remove(&evt->wait_queue, tcb);
            tcb->cont.result = KERN_OK;
            sched_wakeup(tcb, KERN_OK);
        }

        tcb = next;
    }

    irq_spin_unlock(&event_lock, crit);
    return KERN_OK;
}

kern_err_t event_clear(event_id_t event_id, uint32_t flags) {
    uint32_t crit = irq_spin_lock(&event_lock);

    event_t *evt = get_event(event_id);
    if (evt == NULL) {
        irq_spin_unlock(&event_lock, crit);
        return KERN_ERR_PARAM;
    }

    evt->flags &= ~flags;

    irq_spin_unlock(&event_lock, crit);
    return KERN_OK;
}

uint32_t event_get(event_id_t event_id) {
    uint32_t crit = irq_spin_lock(&event_lock);

    event_t *evt = get_event(event_id);
    if (evt == NULL) {
        irq_spin_unlock(&event_lock, crit);
        return 0;
    }

    uint32_t flags = evt->flags;

    irq_spin_unlock(&event_lock, crit);
    return flags;
}
