/**
 * @file irq.c
 * @brief 中断管理实现 — ISR 注册、线程化 IRQ、上下文检测
 */

#include "irq.h"
#include "endpoint.h"
#include "event.h"
#include "hal.h"
#include "scheduler.h"
#include "task.h"
#include "trace.h"
#include "stats.h"
#include "capability.h"
#include "board_config.h"
#include "spinlock.h"
#include <string.h>

#if TARGET_BOARD == BOARD_RP2350_PICO2 && SMP
#include "hardware/regs/intctrl.h"
#endif

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

static int irq_is_kernel_reserved(int16_t irq) {
#if TARGET_BOARD == BOARD_RP2350_PICO2 && SMP
    /* The SMP scheduler and flash-safe lockout share this FIFO interrupt.
     * Replacing its vector breaks all remote wakeups and can park a task
     * forever.  It is never a user-allocatable IRQ while SMP is enabled. */
    return irq == (int16_t)SIO_IRQ_FIFO;
#else
    (void)irq;
    return 0;
#endif
}

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
    kobject_header_t hdr;       /* M2-Step3d: 对象 header */
    int16_t irq_num;
    uint8_t in_use;
} irq_cap_object_t;

/*============================================================================
 * 静态池
 *============================================================================*/

static irq_desc_t irq_descriptors[IRQ_MAX_USER];
static irq_notify_binding_t irq_notify_bindings[IRQ_MAX_USER];
static irq_spinlock_t irq_table_lock;

/* ISR-safe event (notification) bindings: when an IRQ fires in ISR context,
 * the kernel event_set()s the bound event so a user task wakes. Unlike
 * irq_notify (endpoint path, task-context-only), this is safe from the ISR
 * dispatch because event_set masks IRQs itself. */
typedef struct {
    uint8_t    bound;
    int16_t    irq_num;
    event_id_t event_id;
    uint32_t   flags;
} irq_event_binding_t;

static irq_event_binding_t irq_event_bindings[IRQ_MAX_USER];

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
    uint32_t crit = irq_spin_lock(&irq_table_lock);
    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (!irq_cap_objects[i].in_use &&
            !kobj_generation_is_retired(irq_cap_objects[i].hdr.generation)) {
            /* M2-Step3d: 跨 memset 保留 generation (free 时已 bump) */
            uint32_t saved_gen = irq_cap_objects[i].hdr.generation;
            memset(&irq_cap_objects[i], 0, sizeof(irq_cap_objects[i]));
            kobj_header_init(&irq_cap_objects[i].hdr, CAP_OBJ_IRQ);
            if (saved_gen != 0) {
                irq_cap_objects[i].hdr.generation = saved_gen;
            }
            irq_cap_objects[i].in_use = 1U;
            irq_spin_unlock(&irq_table_lock, crit);
            return &irq_cap_objects[i];
        }
    }
    irq_spin_unlock(&irq_table_lock, crit);
    return NULL;
}

static void irq_cap_free_object(irq_cap_object_t *obj) {
    if (obj != NULL) {
        uint32_t crit = irq_spin_lock(&irq_table_lock);
        /* M2-Step3d: bump generation 跨 memset 保留 */
        uint32_t next_gen = kobj_header_prepare_reuse(&obj->hdr);
        memset(obj, 0, sizeof(*obj));
        obj->hdr.obj_type   = CAP_OBJ_IRQ;
        obj->hdr.generation = next_gen;
        irq_spin_unlock(&irq_table_lock, crit);
    }
}

static void irq_clear_endpoint_binding(int16_t irq) {
    uint32_t crit = irq_spin_lock(&irq_table_lock);
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
    irq_spin_unlock(&irq_table_lock, crit);
}

static void irq_clear_endpoint_binding_for_cap(cap_id_t cap) {
    uint32_t crit = irq_spin_lock(&irq_table_lock);
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
    irq_spin_unlock(&irq_table_lock, crit);
}

static void irq_set_endpoint_binding_cap(int16_t irq, cap_id_t cap) {
    uint32_t crit = irq_spin_lock(&irq_table_lock);
    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (irq_notify_bindings[i].bound &&
            irq_notify_bindings[i].irq_num == irq) {
            irq_notify_bindings[i].bound_cap = cap;
            break;
        }
    }
    irq_spin_unlock(&irq_table_lock, crit);
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
    irq_spin_init_rank(&irq_table_lock, LOCKDEP_RANK_REGISTRY);
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
    if (irq_is_kernel_reserved(irq)) {
        irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_ERR_PERM,
                         STATS_COUNTER_BUSY);
        return KERN_ERR_PERM;
    }

    uint32_t crit = irq_spin_lock(&irq_table_lock);

    /* 检查是否已注册 */
    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (irq_descriptors[i].in_use && irq_descriptors[i].irq_num == irq) {
            irq_spin_unlock(&irq_table_lock, crit);
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
        irq_spin_unlock(&irq_table_lock, crit);
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

    irq_spin_unlock(&irq_table_lock, crit);
    irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_OK, STATS_COUNTER_OK);
    return KERN_OK;
}

kern_err_t irq_unregister(int16_t irq) {
    if (irq < 0 || irq >= IRQ_COUNT_MAX) {
        irq_record_event(irq, TRACE_IRQ_RELEASE, KERN_ERR_PARAM,
                         STATS_COUNTER_ERROR);
        return KERN_ERR_PARAM;
    }
    if (irq_is_kernel_reserved(irq)) {
        return KERN_ERR_PERM;
    }

    uint32_t crit = irq_spin_lock(&irq_table_lock);

    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (irq_descriptors[i].in_use && irq_descriptors[i].irq_num == irq) {
            hal_irq_disable_irq((uint32_t)irq);
            hal_irq_set_vector((uint32_t)irq, &_default_handler);
            memset(&irq_descriptors[i], 0, sizeof(irq_desc_t));
            irq_spin_unlock(&irq_table_lock, crit);
            irq_record_event(irq, TRACE_IRQ_RELEASE, KERN_OK,
                             STATS_COUNTER_DELETE);
            return KERN_OK;
        }
    }

    irq_spin_unlock(&irq_table_lock, crit);
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
    if (irq_is_kernel_reserved(irq)) {
        return KERN_ERR_PERM;
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
    if (irq_is_kernel_reserved(irq)) {
        return KERN_ERR_PERM;
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
    if (irq_is_kernel_reserved(irq)) {
        return KERN_ERR_PERM;
    }
    if (!endpoint_exists(ep_id)) {
        irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_ERR_PARAM,
                         STATS_COUNTER_ERROR);
        return KERN_ERR_PARAM;
    }

    uint32_t crit = irq_spin_lock(&irq_table_lock);

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
        irq_spin_unlock(&irq_table_lock, crit);
        irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_ERR_RESOURCE,
                         STATS_COUNTER_QUEUE_FULL);
        return KERN_ERR_RESOURCE;
    }

    irq_notify_bindings[slot].irq_num = irq;
    irq_notify_bindings[slot].ep_id = ep_id;
    irq_notify_bindings[slot].bound_cap = KERN_INVALID_ID;
    irq_notify_bindings[slot].badge = badge;
    irq_notify_bindings[slot].bound = 1;

    irq_spin_unlock(&irq_table_lock, crit);
    irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_OK, STATS_COUNTER_OK);
    return KERN_OK;
}

/* Fire any event binding for `irq` from ISR context. Called by the ISR
 * dispatch path. event_set is ISR-safe (masks IRQs). */
static void irq_event_fire(int16_t irq) {
    event_id_t event_id = KERN_INVALID_ID;
    uint32_t flags = 0U;
    uint32_t crit = irq_spin_lock(&irq_table_lock);
    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (irq_event_bindings[i].bound &&
            irq_event_bindings[i].irq_num == irq) {
            event_id = irq_event_bindings[i].event_id;
            flags = irq_event_bindings[i].flags;
            break;
        }
    }
    irq_spin_unlock(&irq_table_lock, crit);
    if (event_id != KERN_INVALID_ID) {
        (void)event_set(event_id, flags);
    }
}

kern_err_t irq_bind_event(int16_t irq, event_id_t event_id, uint32_t flags) {
    if (irq < 0 || irq >= IRQ_COUNT_MAX) {
        return KERN_ERR_PARAM;
    }
    uint32_t crit = irq_spin_lock(&irq_table_lock);
    int slot = -1;
    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (irq_event_bindings[i].bound &&
            irq_event_bindings[i].irq_num == irq) {
            slot = i;  /* refresh existing */
            break;
        }
        if (slot < 0 && !irq_event_bindings[i].bound) {
            slot = i;
        }
    }
    if (slot < 0) {
        irq_spin_unlock(&irq_table_lock, crit);
        return KERN_ERR_RESOURCE;
    }
    irq_event_bindings[slot].bound   = 1;
    irq_event_bindings[slot].irq_num = irq;
    irq_event_bindings[slot].event_id = event_id;
    irq_event_bindings[slot].flags   = flags;
    irq_spin_unlock(&irq_table_lock, crit);
    return KERN_OK;
}

kern_err_t irq_unbind_event(int16_t irq) {
    uint32_t crit = irq_spin_lock(&irq_table_lock);
    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (irq_event_bindings[i].bound &&
            irq_event_bindings[i].irq_num == irq) {
            irq_event_bindings[i].bound = 0;
            irq_event_bindings[i].irq_num = KERN_INVALID_ID;
            irq_event_bindings[i].event_id = KERN_INVALID_ID;
            irq_event_bindings[i].flags = 0;
        }
    }
    irq_spin_unlock(&irq_table_lock, crit);
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

    uint32_t crit = irq_spin_lock(&irq_table_lock);
    for (int i = 0; i < IRQ_MAX_USER; i++) {
        if (irq_notify_bindings[i].bound &&
            irq_notify_bindings[i].irq_num == irq) {
            ep_id = irq_notify_bindings[i].ep_id;
            badge = irq_notify_bindings[i].badge;
            break;
        }
    }
    irq_spin_unlock(&irq_table_lock, crit);

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
    if (irq_is_kernel_reserved(irq)) {
        return KERN_ERR_PERM;
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
    /* M2-Step3d: 启用 obj_generation cross-check */
    cap_id_t cap = cap_create_for_gen(NULL, obj, CAP_OBJ_IRQ, rights,
                                      obj->hdr.generation);
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

kern_err_t kirq_bind_event(cap_id_t cap, event_id_t event_id, uint32_t flags) {
    irq_cap_register_hooks();

    irq_cap_object_t *obj = cap_resolve(cap, CAP_OBJ_IRQ, CAP_WRITE);
    if (obj == NULL) {
        return KERN_ERR_CAP;
    }
    return irq_bind_event(obj->irq_num, event_id, flags);
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

    /* ISR-safe event notification path: if this IRQ has an event (notification)
     * binding, signal the waiting user task now (event_set masks IRQs). */
    irq_event_fire((int16_t)irq);

    task_id_t wake_task = KERN_INVALID_ID;
    uint32_t crit = irq_spin_lock(&irq_table_lock);
    for (int i = 0; i < IRQ_THREADED_MAX; i++) {
        irq_thread_t *it = &irq_threads[i];
        if (!it->in_use) continue;
        if (it->stopping) continue;
        if (it->irq_num != (int16_t)irq) continue;

        /* 标记待处理 (ISR 上下文无需临界区) */
        it->pending = 1;
        wake_task = it->task_id;
        break;
    }
    irq_spin_unlock(&irq_table_lock, crit);
    if (wake_task != KERN_INVALID_ID) {
        tcb_t *tcb = task_get_tcb(wake_task);
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
    uint32_t crit = irq_spin_lock(&irq_table_lock);
        if (it->stopping) {
            memset(it, 0, sizeof(irq_thread_t));
            irq_spin_unlock(&irq_table_lock, crit);
            task_exit(NULL);
        }
        int was_pending = it->pending;
        it->pending = 0;
        irq_spin_unlock(&irq_table_lock, crit);

        if (!was_pending) {
            /* 阻塞等待 ISR 唤醒，1 tick 超时保护竞争窗口 */
            sched_block(BLOCK_REASON_IRQ, it, 1);
        }

        crit = irq_spin_lock(&irq_table_lock);
        task_func_t handler = it->handler;
        void *handler_arg = it->arg;
        int16_t irq_num = it->irq_num;
        if (handler != NULL && !it->stopping) {
            it->running = 1;
        }
        irq_spin_unlock(&irq_table_lock, crit);

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

        crit = irq_spin_lock(&irq_table_lock);
        it->running = 0;
        if (it->stopping) {
            memset(it, 0, sizeof(irq_thread_t));
            irq_spin_unlock(&irq_table_lock, crit);
            task_exit(NULL);
        }
        irq_num = it->irq_num;
        irq_spin_unlock(&irq_table_lock, crit);

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
    if (irq_is_kernel_reserved(irq)) {
        return KERN_ERR_PERM;
    }

    uint32_t crit = irq_spin_lock(&irq_table_lock);

    /* 检查是否已注册 */
    for (int i = 0; i < IRQ_THREADED_MAX; i++) {
        if (irq_threads[i].in_use && irq_threads[i].irq_num == irq) {
            irq_spin_unlock(&irq_table_lock, crit);
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
        irq_spin_unlock(&irq_table_lock, crit);
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
        irq_spin_unlock(&irq_table_lock, crit);
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

    irq_spin_unlock(&irq_table_lock, crit);
    irq_record_event(irq, TRACE_IRQ_REGISTER, KERN_OK, STATS_COUNTER_OK);
    return KERN_OK;
}

kern_err_t irq_release_threaded(int16_t irq) {
    if (irq < 0 || irq >= IRQ_COUNT_MAX) {
        irq_record_event(irq, TRACE_IRQ_RELEASE, KERN_ERR_PARAM,
                         STATS_COUNTER_ERROR);
        return KERN_ERR_PARAM;
    }
    if (irq_is_kernel_reserved(irq)) {
        return KERN_ERR_PERM;
    }

    uint32_t crit = irq_spin_lock(&irq_table_lock);

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
            irq_spin_unlock(&irq_table_lock, crit);
            irq_record_event(irq, TRACE_IRQ_RELEASE, KERN_OK,
                             STATS_COUNTER_DELETE);
            return KERN_OK;
        }

        /* Publish the slot as stopped before dropping the table lock.  Task
         * teardown may execute capability hooks that acquire this lock. */
        task_id_t task_id = it->task_id;
        memset(it, 0, sizeof(irq_thread_t));
        irq_spin_unlock(&irq_table_lock, crit);
        (void)task_delete(task_id);
        irq_record_event(irq, TRACE_IRQ_RELEASE, KERN_OK,
                         STATS_COUNTER_DELETE);
        return KERN_OK;
    }

    irq_spin_unlock(&irq_table_lock, crit);
    irq_record_event(irq, TRACE_IRQ_RELEASE, KERN_ERR_NOEXIST,
                     STATS_COUNTER_NOEXIST);
    return KERN_ERR_NOEXIST;
}

#endif /* IRQ_THREADED_ENABLE */
