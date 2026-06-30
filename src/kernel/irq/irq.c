/**
 * @file irq.c
 * @brief 中断管理实现 — ISR 注册、线程化 IRQ、上下文检测
 */

#include "irq.h"
#include "endpoint.h"
#include "hal.h"
#include "scheduler.h"
#include "task.h"
#include "trace.h"
#include "stats.h"
#include "capability.h"
#include "board_config.h"
#include <string.h>

#ifndef TRACE_IRQ_REGISTER
#define TRACE_IRQ_REGISTER    1
#define TRACE_IRQ_FIRE        2
#define TRACE_IRQ_RELEASE     3
#define TRACE_IRQ_SPURIOUS    4
#define TRACE_IRQ_MASK        5
#define TRACE_IRQ_UNMASK      6
#endif

#ifndef STATS_COUNTER_OK
#define STATS_COUNTER_OK         0
#define STATS_COUNTER_ERROR      1
#define STATS_COUNTER_QUEUE_FULL 2
#define STATS_COUNTER_DELETE     4
#define STATS_COUNTER_BUSY       6
#define STATS_COUNTER_NOEXIST    7
#endif

/*============================================================================
 * 内部类型和常量
 *============================================================================*/

#define IRQ_FLAG_HANDLER    0x01
#define IRQ_FLAG_THREADED   0x02

#define IRQ_COUNT_MAX       ((int16_t)BOARD_IRQ_COUNT)

typedef struct {
    int16_t     irq_num;
    isr_func_t  handler;
    uint8_t     priority;
    uint8_t     flags;
    uint8_t     in_use;
} irq_desc_t;

typedef struct {
    int16_t  irq_num;
    ep_id_t  ep_id;
    cap_id_t bound_cap;
    uint32_t badge;
    uint8_t  bound;
} irq_notify_binding_t;

typedef struct {
    int16_t irq_num;
    uint8_t in_use;
} irq_cap_object_t;

/*============================================================================
 * 静态池
 *============================================================================*/

static irq_desc_t irq_descriptors[IRQ_MAX_USER];
static irq_notify_binding_t irq_notify_bindings[IRQ_MAX_USER];
#if CAP_ENABLE
static irq_cap_object_t irq_cap_objects[IRQ_MAX_USER];
#endif

#if IRQ_THREADED_ENABLE
static irq_thread_t irq_threads[IRQ_THREADED_MAX];

extern void _default_handler(void);

static void _threaded_isr_dispatch(void);
static void _threaded_irq_task(void *arg);
#endif

/*============================================================================
 * Trace / Stats
 *============================================================================*/

static uint8_t irq_current_task_id(void) {
    tcb_t *current = sched_get_current();
    return current ? (uint8_t)current->id : 0xFFU;
}

#if TRACE_ENABLE
static uint8_t irq_trace_result(kern_err_t err) {
    switch (err) {
        case KERN_OK:
            return TRACE_RESULT_OK;
        case KERN_ERR_BUSY:
            return TRACE_RESULT_BUSY;
        case KERN_ERR_NOEXIST:
            return TRACE_RESULT_NOEXIST;
        case KERN_ERR_RESOURCE:
        case KERN_ERR_OVERFLOW:
            return TRACE_RESULT_FULL;
        default:
            return TRACE_RESULT_ERR;
    }
}
#endif

static void irq_record_event(int16_t irq, uint8_t action,
                             kern_err_t err, uint8_t counter) {
#if TRACE_ENABLE
    uint8_t object_id = (irq >= 0) ? (uint8_t)irq : 0xFFU;
    trace_irq(irq_current_task_id(), object_id, action, irq_trace_result(err));
#else
    (void)irq;
    (void)action;
#endif

#if KERN_TASK_STATS
    if (err != KERN_OK) {
        if (err == KERN_ERR_NOEXIST) {
            counter = STATS_COUNTER_NOEXIST;
        } else if (err == KERN_ERR_BUSY) {
            counter = STATS_COUNTER_BUSY;
        } else {
            counter = STATS_COUNTER_ERROR;
        }
    }
    (void)stats_record_event(STATS_SUBSYS_IRQ, counter);
#else
    (void)counter;
#endif
    (void)err;
}

#if CAP_ENABLE
static irq_cap_object_t *irq_cap_alloc_object(void) {
    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (!irq_cap_objects[i].in_use) {
            memset(&irq_cap_objects[i], 0, sizeof(irq_cap_objects[i]));
            irq_cap_objects[i].in_use = 1U;
            return &irq_cap_objects[i];
        }
    }
    return NULL;
}

static void irq_cap_free_object(irq_cap_object_t *obj) {
    if (obj != NULL) {
        memset(obj, 0, sizeof(*obj));
    }
}

static void irq_clear_endpoint_binding(int16_t irq) {
    uint32_t crit = hal_enter_critical();
    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (irq_notify_bindings[i].bound &&
            irq_notify_bindings[i].irq_num == irq) {
            irq_notify_bindings[i].irq_num = KERN_INVALID_ID;
            irq_notify_bindings[i].ep_id = KERN_INVALID_ID;
            irq_notify_bindings[i].bound_cap = KERN_INVALID_ID;
            irq_notify_bindings[i].badge = 0;
            irq_notify_bindings[i].bound = 0;
        }
    }
    hal_exit_critical(crit);
}

static void irq_clear_endpoint_binding_for_cap(cap_id_t cap) {
    uint32_t crit = hal_enter_critical();
    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (irq_notify_bindings[i].bound &&
            irq_notify_bindings[i].bound_cap == cap) {
            irq_notify_bindings[i].irq_num = KERN_INVALID_ID;
            irq_notify_bindings[i].ep_id = KERN_INVALID_ID;
            irq_notify_bindings[i].bound_cap = KERN_INVALID_ID;
            irq_notify_bindings[i].badge = 0;
            irq_notify_bindings[i].bound = 0;
        }
    }
    hal_exit_critical(crit);
}

static void irq_set_endpoint_binding_cap(int16_t irq, cap_id_t cap) {
    uint32_t crit = hal_enter_critical();
    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (irq_notify_bindings[i].bound &&
            irq_notify_bindings[i].irq_num == irq) {
            irq_notify_bindings[i].bound_cap = cap;
            break;
        }
    }
    hal_exit_critical(crit);
}

static void irq_cap_cleanup(void *object, uint8_t obj_type) {
    if (obj_type == CAP_OBJ_IRQ && object != NULL) {
        irq_cap_object_t *obj = (irq_cap_object_t *)object;
        irq_clear_endpoint_binding(obj->irq_num);
        irq_cap_free_object(obj);
    }
}

static void irq_cap_revoke_hook(cap_id_t cap, void *object, uint8_t obj_type) {
    (void)object;
    if (obj_type == CAP_OBJ_IRQ) {
        irq_clear_endpoint_binding_for_cap(cap);
    }
}

static void irq_cap_register_hooks(void) {
    (void)cap_register_cleanup(CAP_OBJ_IRQ, irq_cap_cleanup);
    (void)cap_register_revoke_hook(CAP_OBJ_IRQ, irq_cap_revoke_hook);
}
#endif

/*============================================================================
 * 初始化
 *============================================================================*/

void irq_init(void) {
    memset(irq_descriptors, 0, sizeof(irq_descriptors));
    memset(irq_notify_bindings, 0, sizeof(irq_notify_bindings));
#if CAP_ENABLE
    memset(irq_cap_objects, 0, sizeof(irq_cap_objects));
    irq_cap_register_hooks();
#endif
    for (int i = 0; i < IRQ_MAX_USER; i++) {
        irq_notify_bindings[i].irq_num = KERN_INVALID_ID;
        irq_notify_bindings[i].ep_id = KERN_INVALID_ID;
        irq_notify_bindings[i].bound_cap = KERN_INVALID_ID;
    }
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
        irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_ERR_PARAM,
                         STATS_COUNTER_ERROR);
        return KERN_ERR_PARAM;
    }
    if (priority > 14) {
        irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_ERR_PARAM,
                         STATS_COUNTER_ERROR);
        return KERN_ERR_PARAM;
    }

    uint32_t crit = hal_enter_critical();

    /* 检查是否已注册 */
    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (irq_descriptors[i].in_use && irq_descriptors[i].irq_num == irq) {
            hal_exit_critical(crit);
            irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_ERR_BUSY,
                             STATS_COUNTER_BUSY);
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
        irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_ERR_RESOURCE,
                         STATS_COUNTER_QUEUE_FULL);
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
    irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_OK, STATS_COUNTER_OK);
    return KERN_OK;
}

kern_err_t irq_unregister(int16_t irq) {
    if (irq < 0 || irq >= IRQ_COUNT_MAX) {
        irq_record_event(irq, TRACE_IRQ_RELEASE, KERN_ERR_PARAM,
                         STATS_COUNTER_ERROR);
        return KERN_ERR_PARAM;
    }

    uint32_t crit = hal_enter_critical();

    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (irq_descriptors[i].in_use && irq_descriptors[i].irq_num == irq) {
            hal_irq_disable_irq((uint32_t)irq);
            hal_irq_set_vector((uint32_t)irq, &_default_handler);
            memset(&irq_descriptors[i], 0, sizeof(irq_desc_t));
            hal_exit_critical(crit);
            irq_record_event(irq, TRACE_IRQ_RELEASE, KERN_OK,
                             STATS_COUNTER_DELETE);
            return KERN_OK;
        }
    }

    hal_exit_critical(crit);
    irq_record_event(irq, TRACE_IRQ_RELEASE, KERN_ERR_NOEXIST,
                     STATS_COUNTER_NOEXIST);
    return KERN_ERR_NOEXIST;
}

kern_err_t irq_enable(int16_t irq) {
    if (irq < 0 || irq >= IRQ_COUNT_MAX) {
        irq_record_event(irq, TRACE_IRQ_UNMASK, KERN_ERR_PARAM,
                         STATS_COUNTER_ERROR);
        return KERN_ERR_PARAM;
    }
    hal_irq_enable_irq((uint32_t)irq);
    irq_record_event(irq, TRACE_IRQ_UNMASK, KERN_OK, STATS_COUNTER_OK);
    return KERN_OK;
}

kern_err_t irq_disable(int16_t irq) {
    if (irq < 0 || irq >= IRQ_COUNT_MAX) {
        irq_record_event(irq, TRACE_IRQ_MASK, KERN_ERR_PARAM,
                         STATS_COUNTER_ERROR);
        return KERN_ERR_PARAM;
    }
    hal_irq_disable_irq((uint32_t)irq);
    irq_record_event(irq, TRACE_IRQ_MASK, KERN_OK, STATS_COUNTER_OK);
    return KERN_OK;
}

kern_err_t irq_bind_endpoint(int16_t irq, ep_id_t ep_id, uint32_t badge) {
    if (irq < 0 || irq >= IRQ_COUNT_MAX) {
        irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_ERR_PARAM,
                         STATS_COUNTER_ERROR);
        return KERN_ERR_PARAM;
    }
    if (!endpoint_exists(ep_id)) {
        irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_ERR_PARAM,
                         STATS_COUNTER_ERROR);
        return KERN_ERR_PARAM;
    }

    uint32_t crit = hal_enter_critical();

    int slot = -1;
    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (irq_notify_bindings[i].bound &&
            irq_notify_bindings[i].irq_num == irq) {
            slot = i;
            break;
        }
        if (slot < 0 && !irq_notify_bindings[i].bound) {
            slot = i;
        }
    }

    if (slot < 0) {
        hal_exit_critical(crit);
        irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_ERR_RESOURCE,
                         STATS_COUNTER_QUEUE_FULL);
        return KERN_ERR_RESOURCE;
    }

    irq_notify_bindings[slot].irq_num = irq;
    irq_notify_bindings[slot].ep_id = ep_id;
    irq_notify_bindings[slot].bound_cap = KERN_INVALID_ID;
    irq_notify_bindings[slot].badge = badge;
    irq_notify_bindings[slot].bound = 1;

    hal_exit_critical(crit);
    irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_OK, STATS_COUNTER_OK);
    return KERN_OK;
}

kern_err_t irq_notify(int16_t irq) {
    if (hal_irq_get_active() >= 0) {
        return KERN_ERR_ISR;
    }
    if (irq < 0 || irq >= IRQ_COUNT_MAX) {
        irq_record_event(irq, TRACE_IRQ_FIRE, KERN_ERR_PARAM,
                         STATS_COUNTER_ERROR);
        return KERN_ERR_PARAM;
    }

    ep_id_t ep_id = KERN_INVALID_ID;
    uint32_t badge = 0;

    uint32_t crit = hal_enter_critical();
    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (irq_notify_bindings[i].bound &&
            irq_notify_bindings[i].irq_num == irq) {
            ep_id = irq_notify_bindings[i].ep_id;
            badge = irq_notify_bindings[i].badge;
            break;
        }
    }
    hal_exit_critical(crit);

    if (ep_id == KERN_INVALID_ID) {
        irq_record_event(irq, TRACE_IRQ_SPURIOUS, KERN_ERR_NOEXIST,
                         STATS_COUNTER_NOEXIST);
        return KERN_ERR_NOEXIST;
    }

    uint8_t msg[KERN_EP_MSG_SIZE];
    memset(msg, 0, sizeof(msg));
    ((uint32_t *)msg)[0] = badge;
    ((uint32_t *)msg)[1] = (uint32_t)irq;

    kern_err_t err = endpoint_notify(ep_id, msg);
    irq_record_event(irq, TRACE_IRQ_FIRE, err,
                     err == KERN_OK ? STATS_COUNTER_OK : STATS_COUNTER_ERROR);
    return err;
}

#if CAP_ENABLE
kern_err_t kirq_create_cap(int16_t irq, uint8_t rights, cap_id_t *out_cap) {
    if (out_cap == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_cap = KERN_INVALID_ID;

    if (irq < 0 || irq >= IRQ_COUNT_MAX) {
        irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_ERR_PARAM,
                         STATS_COUNTER_ERROR);
        return KERN_ERR_PARAM;
    }

    if (rights == 0U) {
        rights = CAP_READ | CAP_WRITE | CAP_MANAGE | CAP_TRANSFER;
    }

    irq_cap_object_t *obj = irq_cap_alloc_object();
    if (obj == NULL) {
        irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_ERR_RESOURCE,
                         STATS_COUNTER_QUEUE_FULL);
        return KERN_ERR_RESOURCE;
    }
    obj->irq_num = irq;

    irq_cap_register_hooks();
    cap_id_t cap = cap_create_for(NULL, obj, CAP_OBJ_IRQ, rights);
    if (cap == KERN_INVALID_ID) {
        irq_cap_free_object(obj);
        irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_ERR_RESOURCE,
                         STATS_COUNTER_QUEUE_FULL);
        return KERN_ERR_RESOURCE;
    }

    *out_cap = cap;
    irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_OK, STATS_COUNTER_OK);
    return KERN_OK;
}

kern_err_t kirq_delete_cap(cap_id_t cap) {
    irq_cap_object_t *obj = cap_resolve(cap, CAP_OBJ_IRQ, CAP_MANAGE);
    if (obj == NULL) {
        return KERN_ERR_CAP;
    }

    int16_t irq = obj->irq_num;
    irq_cap_register_hooks();
    kern_err_t err = cap_revoke(cap);
    irq_record_event(irq, TRACE_IRQ_RELEASE, err,
                     err == KERN_OK ? STATS_COUNTER_DELETE :
                                      STATS_COUNTER_ERROR);
    return err;
}

kern_err_t kirq_get_number(cap_id_t cap, int16_t *irq) {
    if (irq == NULL) {
        return KERN_ERR_PARAM;
    }
    *irq = KERN_INVALID_ID;

    irq_cap_object_t *obj = cap_resolve(cap, CAP_OBJ_IRQ, CAP_READ);
    if (obj == NULL) {
        return KERN_ERR_CAP;
    }

    *irq = obj->irq_num;
    return KERN_OK;
}

kern_err_t kirq_bind_endpoint(cap_id_t cap, ep_id_t ep_id, uint32_t badge) {
    irq_cap_register_hooks();

    irq_cap_object_t *obj = cap_resolve(cap, CAP_OBJ_IRQ, CAP_WRITE);
    if (obj == NULL) {
        return KERN_ERR_CAP;
    }

    kern_err_t err = irq_bind_endpoint(obj->irq_num, ep_id, badge);
    if (err == KERN_OK) {
        irq_set_endpoint_binding_cap(obj->irq_num, cap);
    }
    return err;
}
#endif

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
    if (irq < 0) {
        irq_record_event((int16_t)irq, TRACE_IRQ_SPURIOUS, KERN_ERR_NOEXIST,
                         STATS_COUNTER_NOEXIST);
        return;
    }

    hal_irq_disable_irq((uint32_t)irq);
    hal_irq_clear_pending((uint32_t)irq);

    for (int i = 0; i < IRQ_THREADED_MAX; i++) {
        irq_thread_t *it = &irq_threads[i];
        if (!it->in_use) continue;
        if (it->stopping) continue;
        if (it->irq_num != (int16_t)irq) continue;

        /* 标记待处理 (ISR 上下文无需临界区) */
        it->pending = 1;

        tcb_t *tcb = task_get_tcb(it->task_id);
        if (tcb) {
            sched_wakeup(tcb, KERN_OK);
        }
        irq_record_event((int16_t)irq, TRACE_IRQ_FIRE, KERN_OK,
                         STATS_COUNTER_OK);
        return;
    }
    irq_record_event((int16_t)irq, TRACE_IRQ_SPURIOUS, KERN_ERR_NOEXIST,
                     STATS_COUNTER_NOEXIST);
}

/**
 * @brief 线程化 IRQ 任务主循环
 */
static void _threaded_irq_task(void *arg) {
    irq_thread_t *it = (irq_thread_t *)arg;

    while (1) {
        /* 原子检查 pending 标志 */
        uint32_t crit = hal_enter_critical();
        if (it->stopping) {
            memset(it, 0, sizeof(irq_thread_t));
            hal_exit_critical(crit);
            task_exit(NULL);
        }
        int was_pending = it->pending;
        it->pending = 0;
        hal_exit_critical(crit);

        if (!was_pending) {
            /* 阻塞等待 ISR 唤醒，1 tick 超时保护竞争窗口 */
            sched_block(BLOCK_REASON_IRQ, it, 1);
        }

        crit = hal_enter_critical();
        task_func_t handler = it->handler;
        void *handler_arg = it->arg;
        int16_t irq_num = it->irq_num;
        if (handler != NULL && !it->stopping) {
            it->running = 1;
        }
        hal_exit_critical(crit);

        if (handler) {
#if KERN_TASK_STATS
            uint32_t t0 = sched_get_tick_count();
#endif
            trace_record(TRACE_ISR_ENTER, 0, (uint16_t)irq_num);
            handler(handler_arg);
            trace_record(TRACE_ISR_EXIT, 0, (uint16_t)irq_num);
#if KERN_TASK_STATS
            stats_record_irq(sched_get_tick_count() - t0);
#endif
        }

        crit = hal_enter_critical();
        it->running = 0;
        if (it->stopping) {
            memset(it, 0, sizeof(irq_thread_t));
            hal_exit_critical(crit);
            task_exit(NULL);
        }
        irq_num = it->irq_num;
        hal_exit_critical(crit);

        /* 重新使能硬件中断 */
        hal_irq_enable_irq((uint32_t)irq_num);
    }
}

kern_err_t irq_request_threaded(int16_t irq, task_func_t handler,
                                void *arg, uint8_t priority,
                                uint32_t stack_size) {
    if (irq < 0 || irq >= IRQ_COUNT_MAX || handler == NULL) {
        irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_ERR_PARAM,
                         STATS_COUNTER_ERROR);
        return KERN_ERR_PARAM;
    }

    uint32_t crit = hal_enter_critical();

    /* 检查是否已注册 */
    for (int i = 0; i < IRQ_THREADED_MAX; i++) {
        if (irq_threads[i].in_use && irq_threads[i].irq_num == irq) {
            hal_exit_critical(crit);
            irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_ERR_BUSY,
                             STATS_COUNTER_BUSY);
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
        irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_ERR_RESOURCE,
                         STATS_COUNTER_QUEUE_FULL);
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
        irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_ERR_RESOURCE,
                         STATS_COUNTER_QUEUE_FULL);
        return KERN_ERR_RESOURCE;
    }

    it->task_id  = tid;
    it->irq_num  = irq;
    it->handler  = handler;
    it->arg      = arg;
    it->priority = priority;
    it->in_use   = 1;
    it->pending  = 0;
    it->running  = 0;
    it->stopping = 0;

    /* 注册 ISR (所有线程化 IRQ 共用一个调度桩) */
    hal_irq_set_vector((uint32_t)irq, _threaded_isr_dispatch);
    hal_irq_set_priority((uint32_t)irq, priority);
    hal_irq_enable_irq((uint32_t)irq);

    hal_exit_critical(crit);
    irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_OK, STATS_COUNTER_OK);
    return KERN_OK;
}

kern_err_t irq_release_threaded(int16_t irq) {
    if (irq < 0 || irq >= IRQ_COUNT_MAX) {
        irq_record_event(irq, TRACE_IRQ_RELEASE, KERN_ERR_PARAM,
                         STATS_COUNTER_ERROR);
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

        it->pending = 0;
        it->stopping = 1;
        it->handler = NULL;
        it->arg = NULL;

        tcb_t *tcb = task_get_tcb(it->task_id);
        if (tcb == sched_get_current()) {
            hal_exit_critical(crit);
            irq_record_event(irq, TRACE_IRQ_RELEASE, KERN_OK,
                             STATS_COUNTER_DELETE);
            return KERN_OK;
        }

        /* 删除关联任务 */
        task_delete(it->task_id);

        memset(it, 0, sizeof(irq_thread_t));
        hal_exit_critical(crit);
        irq_record_event(irq, TRACE_IRQ_RELEASE, KERN_OK,
                         STATS_COUNTER_DELETE);
        return KERN_OK;
    }

    hal_exit_critical(crit);
    irq_record_event(irq, TRACE_IRQ_RELEASE, KERN_ERR_NOEXIST,
                     STATS_COUNTER_NOEXIST);
    return KERN_ERR_NOEXIST;
}

#endif /* IRQ_THREADED_ENABLE */
