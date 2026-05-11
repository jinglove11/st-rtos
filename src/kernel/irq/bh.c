/**
 * @file bh.c
 * @brief 底半部 (Bottom Half) 实现 — ISR 安全延迟处理
 */

#include "bh.h"
#include "hal.h"
#include "task.h"
#include "semaphore.h"
#include "trace.h"
#include <string.h>

/*============================================================================
 * 静态池
 *============================================================================*/

#if IRQ_BH_ENABLE
static bh_t bh_pool[IRQ_BH_MAX];
static sem_id_t bh_sem = KERN_INVALID_ID;
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
                hal_exit_critical(crit);

                if (handler) {
                    handler(bh_arg);
                }

                crit = hal_enter_critical();
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
        return KERN_INVALID_ID;
    }

    uint32_t crit = hal_enter_critical();

    for (int i = 0; i < IRQ_BH_MAX; i++) {
        if (!bh_pool[i].in_use) {
            bh_pool[i].handler = handler;
            bh_pool[i].arg     = arg;
            bh_pool[i].pending = 0;
            bh_pool[i].in_use  = 1;
            hal_exit_critical(crit);
            return (int16_t)i;
        }
    }

    hal_exit_critical(crit);
    return KERN_INVALID_ID;
}

kern_err_t bh_schedule(int16_t bh_id) {
    if (bh_id < 0 || bh_id >= IRQ_BH_MAX) {
        return KERN_ERR_PARAM;
    }

    /* ISR 安全: 仅设置标志位 */
    bh_t *bh = &bh_pool[bh_id];
    if (!bh->in_use) {
        return KERN_ERR_NOEXIST;
    }

    bh->pending = 1;
    trace_record(TRACE_BH_SCHEDULE, 0, (uint16_t)bh_id);
    sem_post(bh_sem);
    return KERN_OK;
}

kern_err_t bh_delete(int16_t bh_id) {
    if (bh_id < 0 || bh_id >= IRQ_BH_MAX) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = hal_enter_critical();
    if (!bh_pool[bh_id].in_use) {
        hal_exit_critical(crit);
        return KERN_ERR_NOEXIST;
    }

    memset(&bh_pool[bh_id], 0, sizeof(bh_t));
    hal_exit_critical(crit);
    return KERN_OK;
}

#else

/* 空实现 (IRQ_BH_ENABLE 关闭时) */
int16_t bh_create(bh_handler_t handler, void *arg) {
    (void)handler; (void)arg;
    return KERN_INVALID_ID;
}
kern_err_t bh_schedule(int16_t bh_id)  { (void)bh_id; return KERN_ERR; }
kern_err_t bh_delete(int16_t bh_id)    { (void)bh_id; return KERN_ERR; }

#endif /* IRQ_BH_ENABLE */
