/**
 * @file event.c
 * @brief 事件标志组实现
 */

#include "event.h"
#include "scheduler.h"
#include "kernel_config.h"
#include "hal.h"
#include <string.h>

/*============================================================================
 * 事件等待控制块 (存储在任务的 block_obj 中)
 *============================================================================*/

typedef struct {
    uint32_t    wait_flags;
    uint32_t    wait_opt;
    uint32_t   *received;
} event_wait_t;

static event_wait_t event_wait_info[KERN_MAX_TASKS];

/*============================================================================
 * 静态分配的事件标志组池
 *============================================================================*/

static event_t event_pool[KERN_MAX_EVENTS];
static uint32_t event_used_bitmap;

/*============================================================================
 * 内部函数
 *============================================================================*/

// 分配事件标志组 ID
static event_id_t alloc_event_id(void) {
    for (int i = 0; i < KERN_MAX_EVENTS; i++) {
        if (!(event_used_bitmap & (1U << i))) {
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

// 初始化等待队列
static void wait_queue_init(wait_queue_t *wq) {
    wq->head = NULL;
    wq->tail = NULL;
    wq->count = 0;
}

// 添加任务到等待队列尾部
static void wait_queue_add(wait_queue_t *wq, tcb_t *tcb) {
    tcb->next = NULL;
    tcb->prev = wq->tail;

    if (wq->tail) {
        wq->tail->next = tcb;
    } else {
        wq->head = tcb;
    }
    wq->tail = tcb;
    wq->count++;
}

// 从等待队列移除任务
static void wait_queue_remove(wait_queue_t *wq, tcb_t *tcb) {
    if (tcb->prev) {
        tcb->prev->next = tcb->next;
    } else {
        wq->head = tcb->next;
    }

    if (tcb->next) {
        tcb->next->prev = tcb->prev;
    } else {
        wq->tail = tcb->prev;
    }

    tcb->next = NULL;
    tcb->prev = NULL;
    wq->count--;
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
    memset(event_pool, 0, sizeof(event_pool));
    event_used_bitmap = 0;
}

event_id_t event_create(uint32_t initial_flags) {
    uint32_t crit = hal_irq_save();

    event_id_t id = alloc_event_id();
    if (id == KERN_INVALID_ID) {
        hal_irq_restore(crit);
        return KERN_INVALID_ID;
    }

    event_t *evt = &event_pool[id];
    evt->flags = initial_flags;
    evt->in_use = 1;
    wait_queue_init(&evt->wait_queue);

    hal_irq_restore(crit);
    return id;
}

kern_err_t event_delete(event_id_t event_id) {
    uint32_t crit = hal_irq_save();

    event_t *evt = get_event(event_id);
    if (evt == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    // 唤醒所有等待的任务
    tcb_t *tcb = evt->wait_queue.head;
    while (tcb) {
        tcb_t *next = tcb->next;
        tcb->block_result = KERN_ERR_NOEXIST;
        sched_wakeup(tcb, KERN_ERR_NOEXIST);
        tcb = next;
    }

    // 清零并释放
    memset(evt, 0, sizeof(event_t));
    free_event_id(event_id);

    hal_irq_restore(crit);
    return KERN_OK;
}

kern_err_t event_wait(event_id_t event_id, uint32_t flags, uint32_t opt,
                      uint32_t timeout, uint32_t *received) {
    uint32_t crit = hal_irq_save();

    event_t *evt = get_event(event_id);
    if (evt == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    if (event_check(evt->flags, flags, opt)) {
        if (received) {
            *received = evt->flags;
        }
        if (opt & EVENT_OPT_CLEAR) {
            evt->flags &= ~flags;
        }
        hal_irq_restore(crit);
        return KERN_OK;
    }

    if (timeout == 0) {
        hal_irq_restore(crit);
        return KERN_ERR_TIMEOUT;
    }

    tcb_t *current = sched_get_current();
    current->block_reason = BLOCK_REASON_EVENT;
    current->block_obj = evt;

    task_id_t tid = current->id;
    if (tid >= 0 && tid < KERN_MAX_TASKS) {
        event_wait_info[tid].wait_flags = flags;
        event_wait_info[tid].wait_opt = opt;
        event_wait_info[tid].received = received;
    }

    wait_queue_add(&evt->wait_queue, current);

    hal_irq_restore(crit);

    kern_err_t result = sched_block(BLOCK_REASON_EVENT, evt, timeout);

    if (result == KERN_OK) {
        crit = hal_irq_save();
        if (received) {
            *received = evt->flags;
        }
        if (opt & EVENT_OPT_CLEAR) {
            evt->flags &= ~flags;
        }
        hal_irq_restore(crit);
    } else {
        crit = hal_irq_save();
        if (current->block_obj == evt) {
            wait_queue_remove(&evt->wait_queue, current);
            current->block_obj = NULL;
        }
        hal_irq_restore(crit);
    }

    return result;
}

kern_err_t event_set(event_id_t event_id, uint32_t flags) {
    uint32_t crit = hal_irq_save();

    event_t *evt = get_event(event_id);
    if (evt == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    evt->flags |= flags;

    tcb_t *tcb = evt->wait_queue.head;
    while (tcb) {
        tcb_t *next = tcb->next;

        task_id_t tid = tcb->id;
        uint32_t wait_flags = 0;
        uint32_t wait_opt = 0;

        if (tid >= 0 && tid < KERN_MAX_TASKS) {
            wait_flags = event_wait_info[tid].wait_flags;
            wait_opt = event_wait_info[tid].wait_opt;
        }

        if (event_check(evt->flags, wait_flags, wait_opt)) {
            wait_queue_remove(&evt->wait_queue, tcb);
            tcb->block_result = KERN_OK;
            sched_wakeup(tcb, KERN_OK);
        }

        tcb = next;
    }

    hal_irq_restore(crit);
    return KERN_OK;
}

kern_err_t event_clear(event_id_t event_id, uint32_t flags) {
    uint32_t crit = hal_irq_save();

    event_t *evt = get_event(event_id);
    if (evt == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    evt->flags &= ~flags;

    hal_irq_restore(crit);
    return KERN_OK;
}

uint32_t event_get(event_id_t event_id) {
    uint32_t crit = hal_irq_save();

    event_t *evt = get_event(event_id);
    if (evt == NULL) {
        hal_irq_restore(crit);
        return 0;
    }

    uint32_t flags = evt->flags;

    hal_irq_restore(crit);
    return flags;
}
