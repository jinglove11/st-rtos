/**
 * @file irq.c
 * @brief 中断管理实现 — ISR 注册、线程化 IRQ、上下文检测
 */

#include "irq.h"
#include "hal.h"
#include "scheduler.h"
#include "task.h"
#include "trace.h"
#include "stats.h"
#include <string.h>

/*============================================================================
 * 内部类型和常量
 *============================================================================*/

#define IRQ_FLAG_HANDLER    0x01
#define IRQ_FLAG_THREADED   0x02

#define IRQ_COUNT_MAX       98   /* STM32F767: WWDG(0) 到 SPDIF_RX(97) */

typedef struct {
    int16_t     irq_num;
    isr_func_t  handler;
    uint8_t     priority;
    uint8_t     flags;
    uint8_t     in_use;
} irq_desc_t;

/*============================================================================
 * 静态池
 *============================================================================*/

static irq_desc_t irq_descriptors[IRQ_MAX_USER];

#if IRQ_THREADED_ENABLE
static irq_thread_t irq_threads[IRQ_THREADED_MAX];

extern void _default_handler(void);

static void _threaded_isr_dispatch(void);
static void _threaded_irq_task(void *arg);
#endif

/*============================================================================
 * 初始化
 *============================================================================*/

void irq_init(void) {
    memset(irq_descriptors, 0, sizeof(irq_descriptors));
#if IRQ_THREADED_ENABLE
    memset(irq_threads, 0, sizeof(irq_threads));
#endif
}

void irq_service_start(void) {
#if IRQ_THREADED_ENABLE
    for (int i = 0; i < IRQ_THREADED_MAX; i++) {
        if (irq_threads[i].in_use) {
            task_start(irq_threads[i].task_id);
        }
    }
#endif
}

/*============================================================================
 * ISR 注册
 *============================================================================*/

kern_err_t irq_register(int16_t irq, isr_func_t handler, uint8_t priority) {
    if (irq < 0 || irq >= IRQ_COUNT_MAX || handler == NULL) {
        return KERN_ERR_PARAM;
    }
    if (priority > 14) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = hal_enter_critical();

    /* 检查是否已注册 */
    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (irq_descriptors[i].in_use && irq_descriptors[i].irq_num == irq) {
            hal_exit_critical(crit);
            return KERN_ERR_BUSY;
        }
    }

    /* 分配槽位 */
    int slot = -1;
    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (!irq_descriptors[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        hal_exit_critical(crit);
        return KERN_ERR_RESOURCE;
    }

    irq_desc_t *desc = &irq_descriptors[slot];
    desc->irq_num  = irq;
    desc->handler  = handler;
    desc->priority = priority;
    desc->flags    = IRQ_FLAG_HANDLER;
    desc->in_use   = 1;

    hal_irq_set_vector((uint32_t)irq, handler);
    hal_irq_set_priority((uint32_t)irq, priority);
    hal_irq_enable_irq((uint32_t)irq);

    hal_exit_critical(crit);
    return KERN_OK;
}

kern_err_t irq_unregister(int16_t irq) {
    if (irq < 0 || irq >= IRQ_COUNT_MAX) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = hal_enter_critical();

    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (irq_descriptors[i].in_use && irq_descriptors[i].irq_num == irq) {
            hal_irq_disable_irq((uint32_t)irq);
            hal_irq_set_vector((uint32_t)irq, &_default_handler);
            memset(&irq_descriptors[i], 0, sizeof(irq_desc_t));
            hal_exit_critical(crit);
            return KERN_OK;
        }
    }

    hal_exit_critical(crit);
    return KERN_ERR_NOEXIST;
}

kern_err_t irq_enable(int16_t irq) {
    if (irq < 0 || irq >= IRQ_COUNT_MAX) {
        return KERN_ERR_PARAM;
    }
    hal_irq_enable_irq((uint32_t)irq);
    return KERN_OK;
}

kern_err_t irq_disable(int16_t irq) {
    if (irq < 0 || irq >= IRQ_COUNT_MAX) {
        return KERN_ERR_PARAM;
    }
    hal_irq_disable_irq((uint32_t)irq);
    return KERN_OK;
}

/*============================================================================
 * 中断上下文检测
 *============================================================================*/

int kern_is_in_isr(void) {
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    return (ipsr & 0x1FF) != 0;
}

int kern_irq_context(void) {
    return (int)hal_irq_get_active();
}

/*============================================================================
 * 线程化 IRQ
 *============================================================================*/

#if IRQ_THREADED_ENABLE

/**
 * @brief 线程化 IRQ 调度桩
 *
 * 所有线程化 IRQ 共享此调度函数。
 * 从 IPSR 读取触发中断号，禁用该 IRQ，唤醒对应任务。
 */
static void _threaded_isr_dispatch(void) {
    int irq = (int)hal_irq_get_active();
    if (irq < 0) return;

    hal_irq_disable_irq((uint32_t)irq);
    hal_irq_clear_pending((uint32_t)irq);

    for (int i = 0; i < IRQ_THREADED_MAX; i++) {
        irq_thread_t *it = &irq_threads[i];
        if (!it->in_use) continue;
        if (it->irq_num != (int16_t)irq) continue;

        /* 标记待处理 (ISR 上下文无需临界区) */
        it->pending = 1;

        tcb_t *tcb = task_get_tcb(it->task_id);
        if (tcb) {
            sched_wakeup(tcb, KERN_OK);
        }
        return;
    }
}

/**
 * @brief 线程化 IRQ 任务主循环
 */
static void _threaded_irq_task(void *arg) {
    irq_thread_t *it = (irq_thread_t *)arg;

    while (1) {
        /* 原子检查 pending 标志 */
        uint32_t crit = hal_enter_critical();
        int was_pending = it->pending;
        it->pending = 0;
        hal_exit_critical(crit);

        if (!was_pending) {
            /* 阻塞等待 ISR 唤醒，1 tick 超时保护竞争窗口 */
            sched_block(BLOCK_REASON_IRQ, it, 1);
        }

        if (it->handler) {
#if KERN_TASK_STATS
            uint32_t t0 = sched_get_tick_count();
#endif
            trace_record(TRACE_ISR_ENTER, 0, (uint16_t)it->irq_num);
            it->handler(it->arg);
            trace_record(TRACE_ISR_EXIT, 0, (uint16_t)it->irq_num);
#if KERN_TASK_STATS
            stats_record_irq(sched_get_tick_count() - t0);
#endif
        }

        /* 重新使能硬件中断 */
        hal_irq_enable_irq((uint32_t)it->irq_num);
    }
}

kern_err_t irq_request_threaded(int16_t irq, task_func_t handler,
                                void *arg, uint8_t priority,
                                uint32_t stack_size) {
    if (irq < 0 || irq >= IRQ_COUNT_MAX || handler == NULL) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = hal_enter_critical();

    /* 检查是否已注册 */
    for (int i = 0; i < IRQ_THREADED_MAX; i++) {
        if (irq_threads[i].in_use && irq_threads[i].irq_num == irq) {
            hal_exit_critical(crit);
            return KERN_ERR_BUSY;
        }
    }

    /* 分配线程化 IRQ 槽位 */
    int slot = -1;
    for (int i = 0; i < IRQ_THREADED_MAX; i++) {
        if (!irq_threads[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        hal_exit_critical(crit);
        return KERN_ERR_RESOURCE;
    }

    irq_thread_t *it = &irq_threads[slot];

    /* 创建任务 (名称必须是唯一的) */
    char name[16];
    {
        int n = 0;
        name[n++] = 'i';
        name[n++] = 'r';
        name[n++] = 'q';
        name[n++] = '_';
        if (irq < 10) {
            name[n++] = (char)('0' + irq);
        } else {
            name[n++] = (char)('0' + irq / 10);
            name[n++] = (char)('0' + irq % 10);
        }
        name[n] = '\0';
    }

    task_id_t tid = task_create(name, _threaded_irq_task, it, priority, stack_size);
    if (tid < 0) {
        hal_exit_critical(crit);
        return KERN_ERR_RESOURCE;
    }

    it->task_id  = tid;
    it->irq_num  = irq;
    it->handler  = handler;
    it->arg      = arg;
    it->priority = priority;
    it->in_use   = 1;

    /* 注册 ISR (所有线程化 IRQ 共用一个调度桩) */
    hal_irq_set_vector((uint32_t)irq, _threaded_isr_dispatch);
    hal_irq_set_priority((uint32_t)irq, priority);
    hal_irq_enable_irq((uint32_t)irq);

    hal_exit_critical(crit);
    return KERN_OK;
}

kern_err_t irq_release_threaded(int16_t irq) {
    if (irq < 0 || irq >= IRQ_COUNT_MAX) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = hal_enter_critical();

    for (int i = 0; i < IRQ_THREADED_MAX; i++) {
        irq_thread_t *it = &irq_threads[i];
        if (!it->in_use || it->irq_num != irq) {
            continue;
        }

        /* 禁用 IRQ，恢复默认 handler */
        hal_irq_disable_irq((uint32_t)irq);
        hal_irq_set_vector((uint32_t)irq, &_default_handler);

        /* 删除关联任务 */
        task_delete(it->task_id);

        memset(it, 0, sizeof(irq_thread_t));
        hal_exit_critical(crit);
        return KERN_OK;
    }

    hal_exit_critical(crit);
    return KERN_ERR_NOEXIST;
}

#endif /* IRQ_THREADED_ENABLE */
