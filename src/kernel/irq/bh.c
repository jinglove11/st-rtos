/**
 * @file bh.c
 * @brief 底半部 (Bottom Half) 实现 — ISR 安全延迟处理
 */

#include "bh.h"
#include "hal.h"
#include "task.h"
#include "scheduler.h"
#include "semaphore.h"
#include "trace.h"
#include "stats.h"
#include <string.h>

#ifndef TRACE_BH_CREATE
#define TRACE_BH_CREATE       1
#define TRACE_BH_SCHED        2
#define TRACE_BH_RUN          3
#define TRACE_BH_CANCEL       4
#define TRACE_BH_DELETE       5
#endif

#ifndef STATS_COUNTER_OK
#define STATS_COUNTER_OK         0
#define STATS_COUNTER_ERROR      1
#define STATS_COUNTER_QUEUE_FULL 2
#define STATS_COUNTER_CANCEL     5
#define STATS_COUNTER_DELETE     4
#define STATS_COUNTER_NOEXIST    7
#endif

/*============================================================================
 * 静态池
 *============================================================================*/

#if IRQ_BH_ENABLE
static bh_t bh_pool[IRQ_BH_MAX];
static sem_id_t bh_sem = KERN_INVALID_ID;
#endif

/*============================================================================
 * Trace / Stats
 *============================================================================*/

#if IRQ_BH_ENABLE

static uint8_t bh_current_task_id(void) {
    tcb_t *current = sched_get_current();
    return current ? (uint8_t)current->id : 0xFFU;
}

#if TRACE_ENABLE
static uint8_t bh_trace_result(kern_err_t err) {
    switch (err) {
        case KERN_OK:
            return TRACE_RESULT_OK;
        case KERN_ERR_NOEXIST:
            return TRACE_RESULT_NOEXIST;
        case KERN_ERR_BUSY:
            return TRACE_RESULT_BUSY;
        case KERN_ERR_RESOURCE:
        case KERN_ERR_OVERFLOW:
            return TRACE_RESULT_FULL;
        default:
            return TRACE_RESULT_ERR;
    }
}
#endif

static void bh_record_event(int16_t bh_id, uint8_t action,
                            kern_err_t err, uint8_t counter) {
#if TRACE_ENABLE
    uint8_t object_id = (bh_id >= 0) ? (uint8_t)bh_id : 0xFFU;
    trace_bh(bh_current_task_id(), object_id, action, bh_trace_result(err));
#else
    (void)bh_id;
    (void)action;
#endif

#if KERN_TASK_STATS
    if (err != KERN_OK) {
        if (err == KERN_ERR_NOEXIST) {
            counter = STATS_COUNTER_NOEXIST;
        } else {
            counter = STATS_COUNTER_ERROR;
        }
    }
    (void)stats_record_event(STATS_SUBSYS_BH, counter);
#else
    (void)counter;
#endif
    (void)err;
}

#endif

/*============================================================================
 * BH 服务任务
 *============================================================================*/

#if IRQ_BH_ENABLE

static void bh_service_loop(void *arg) {
    (void)arg;
    while (1) {
        /* 等待 BH 被调度 (信号量唤醒, 100 tick 安全超时) */
        sem_wait(bh_sem, 100);

        /* 遍历所有 BH，处理 pending 的 */
        uint32_t crit = hal_enter_critical();
        for (int i = 0; i < IRQ_BH_MAX; i++) {
            bh_t *bh = &bh_pool[i];
            if (bh->in_use && bh->pending) {
                bh_handler_t handler = bh->handler;
                void        *bh_arg  = bh->arg;
                bh->pending = 0;
                bh->running = 1;
                hal_exit_critical(crit);

                if (handler) {
                    handler(bh_arg);
                }
                bh_record_event((int16_t)i, TRACE_BH_RUN, KERN_OK,
                                STATS_COUNTER_OK);

                crit = hal_enter_critical();
                bh->running = 0;
                if (bh->delete_pending) {
                    memset(bh, 0, sizeof(bh_t));
                }
            }
        }
        hal_exit_critical(crit);
    }
}

#endif /* IRQ_BH_ENABLE */

/*============================================================================
 * 初始化
 *============================================================================*/

void bh_init(void) {
#if IRQ_BH_ENABLE
    memset(bh_pool, 0, sizeof(bh_pool));
    bh_sem = sem_create(0, 0);  /* 计数信号量, 初始0, 无上限 */
#endif
}

void bh_service_start(void) {
#if IRQ_BH_ENABLE
    /* 创建 BH 服务任务 (优先级 2, 低于定时器服务优先级 1) */
    task_id_t bh_tid = task_create("bh_svc", bh_service_loop, NULL, 2, 512);
    if (bh_tid >= 0) {
        task_start(bh_tid);
    }
#endif
}

/*============================================================================
 * 底半部生命周期
 *============================================================================*/

#if IRQ_BH_ENABLE

int16_t bh_create(bh_handler_t handler, void *arg) {
    if (handler == NULL) {
        bh_record_event(KERN_INVALID_ID, TRACE_BH_CREATE, KERN_ERR_PARAM,
                        STATS_COUNTER_ERROR);
        return KERN_INVALID_ID;
    }

    uint32_t crit = hal_enter_critical();

    for (int i = 0; i < IRQ_BH_MAX; i++) {
        if (!bh_pool[i].in_use) {
            bh_pool[i].handler = handler;
            bh_pool[i].arg     = arg;
            bh_pool[i].pending = 0;
            bh_pool[i].running = 0;
            bh_pool[i].delete_pending = 0;
            bh_pool[i].in_use  = 1;
            hal_exit_critical(crit);
            bh_record_event((int16_t)i, TRACE_BH_CREATE, KERN_OK,
                            STATS_COUNTER_OK);
            return (int16_t)i;
        }
    }

    hal_exit_critical(crit);
    bh_record_event(KERN_INVALID_ID, TRACE_BH_CREATE, KERN_ERR_RESOURCE,
                    STATS_COUNTER_QUEUE_FULL);
    return KERN_INVALID_ID;
}

kern_err_t bh_schedule(int16_t bh_id) {
    if (bh_id < 0 || bh_id >= IRQ_BH_MAX) {
        bh_record_event(bh_id, TRACE_BH_SCHED, KERN_ERR_PARAM,
                        STATS_COUNTER_ERROR);
        return KERN_ERR_PARAM;
    }

    /* ISR 安全: 仅设置标志位 */
    bh_t *bh = &bh_pool[bh_id];
    if (!bh->in_use) {
        bh_record_event(bh_id, TRACE_BH_SCHED, KERN_ERR_NOEXIST,
                        STATS_COUNTER_NOEXIST);
        return KERN_ERR_NOEXIST;
    }

    uint32_t crit = hal_enter_critical();
    if (!bh->in_use || bh->delete_pending) {
        hal_exit_critical(crit);
        bh_record_event(bh_id, TRACE_BH_SCHED, KERN_ERR_NOEXIST,
                        STATS_COUNTER_NOEXIST);
        return KERN_ERR_NOEXIST;
    }

    bh->pending = 1;
    hal_exit_critical(crit);
    trace_record(TRACE_BH_SCHEDULE, 0, (uint16_t)bh_id);
    sem_post(bh_sem);
    bh_record_event(bh_id, TRACE_BH_SCHED, KERN_OK, STATS_COUNTER_OK);
    return KERN_OK;
}

kern_err_t bh_cancel(int16_t bh_id) {
    if (bh_id < 0 || bh_id >= IRQ_BH_MAX) {
        bh_record_event(bh_id, TRACE_BH_CANCEL, KERN_ERR_PARAM,
                        STATS_COUNTER_ERROR);
        return KERN_ERR_PARAM;
    }

    uint32_t crit = hal_enter_critical();
    if (!bh_pool[bh_id].in_use) {
        hal_exit_critical(crit);
        bh_record_event(bh_id, TRACE_BH_CANCEL, KERN_ERR_NOEXIST,
                        STATS_COUNTER_NOEXIST);
        return KERN_ERR_NOEXIST;
    }

    bh_pool[bh_id].pending = 0;
    hal_exit_critical(crit);
    bh_record_event(bh_id, TRACE_BH_CANCEL, KERN_OK, STATS_COUNTER_CANCEL);
    return KERN_OK;
}

kern_err_t bh_delete(int16_t bh_id) {
    if (bh_id < 0 || bh_id >= IRQ_BH_MAX) {
        bh_record_event(bh_id, TRACE_BH_DELETE, KERN_ERR_PARAM,
                        STATS_COUNTER_ERROR);
        return KERN_ERR_PARAM;
    }

    uint32_t crit = hal_enter_critical();
    if (!bh_pool[bh_id].in_use) {
        hal_exit_critical(crit);
        bh_record_event(bh_id, TRACE_BH_DELETE, KERN_ERR_NOEXIST,
                        STATS_COUNTER_NOEXIST);
        return KERN_ERR_NOEXIST;
    }

    bh_pool[bh_id].pending = 0;
    if (bh_pool[bh_id].running) {
        bh_pool[bh_id].delete_pending = 1;
        bh_pool[bh_id].handler = NULL;
        bh_pool[bh_id].arg = NULL;
    } else {
        memset(&bh_pool[bh_id], 0, sizeof(bh_t));
    }
    hal_exit_critical(crit);
    bh_record_event(bh_id, TRACE_BH_DELETE, KERN_OK, STATS_COUNTER_DELETE);
    return KERN_OK;
}

#else

/* 空实现 (IRQ_BH_ENABLE 关闭时) */
int16_t bh_create(bh_handler_t handler, void *arg) {
    (void)handler; (void)arg;
    return KERN_INVALID_ID;
}
kern_err_t bh_schedule(int16_t bh_id)  { (void)bh_id; return KERN_ERR; }
kern_err_t bh_cancel(int16_t bh_id)    { (void)bh_id; return KERN_ERR; }
kern_err_t bh_delete(int16_t bh_id)    { (void)bh_id; return KERN_ERR; }

#endif /* IRQ_BH_ENABLE */
