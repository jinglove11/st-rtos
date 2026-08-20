/**
 * @file factory.c
 * @brief Capability-authorized kernel object creation
 */

#include "factory.h"
#include "channel.h"
#include "endpoint.h"
#include "event.h"
#include "notification.h"
#include "mqueue.h"
#include "mem.h"
#include "mutex.h"
#include "semaphore.h"
#include "spinlock.h"
#include "task.h"
#include "timer.h"
#include <string.h>

#if CAP_ENABLE

typedef struct {
    kobject_header_t hdr;
    uint32_t allowed_mask;
    uint8_t in_use;
} factory_object_t;

static factory_object_t factory_pool[KERN_MAX_FACTORIES];
static irq_spinlock_t factory_lock;

static void factory_release_locked(factory_object_t *factory) {
    uint32_t next_generation = kobj_header_prepare_reuse(&factory->hdr);
    memset(factory, 0, sizeof(*factory));
    factory->hdr.obj_type = CAP_OBJ_FACTORY;
    factory->hdr.generation = next_generation;
}

static void factory_cap_cleanup(void *object, uint8_t obj_type) {
    if (object == NULL || obj_type != CAP_OBJ_FACTORY) {
        return;
    }

    factory_object_t *factory = (factory_object_t *)object;
    uint32_t crit = irq_spin_lock(&factory_lock);
    if (factory >= &factory_pool[0] &&
        factory < &factory_pool[KERN_MAX_FACTORIES] &&
        factory->in_use) {
        factory_release_locked(factory);
    }
    irq_spin_unlock(&factory_lock, crit);
}

void factory_init(void) {
    irq_spin_init_rank(&factory_lock, LOCKDEP_RANK_REGISTRY);
    memset(factory_pool, 0, sizeof(factory_pool));
    for (int i = 0; i < KERN_MAX_FACTORIES; i++) {
        kobj_header_init(&factory_pool[i].hdr, CAP_OBJ_FACTORY);
    }
    (void)cap_register_cleanup(CAP_OBJ_FACTORY, factory_cap_cleanup);
}

uint32_t factory_supported_mask(void) {
    return FACTORY_SUPPORTED_MASK;
}

cap_id_t factory_create_root_cap(tcb_t *owner, uint32_t allowed_mask,
                                 uint8_t rights) {
    allowed_mask &= FACTORY_SUPPORTED_MASK;
    if (owner == NULL || owner->id < 0 || allowed_mask == 0U ||
        (rights & ~CAP_FULL) != 0U || (rights & CAP_WRITE) == 0U) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = irq_spin_lock(&factory_lock);
    factory_object_t *factory = NULL;
    for (int i = 0; i < KERN_MAX_FACTORIES; i++) {
        if (!factory_pool[i].in_use &&
            !kobj_generation_is_retired(factory_pool[i].hdr.generation)) {
            factory = &factory_pool[i];
            break;
        }
    }
    if (factory == NULL) {
        irq_spin_unlock(&factory_lock, crit);
        return KERN_ERR_RESOURCE;
    }

    uint32_t generation = factory->hdr.generation;
    memset(factory, 0, sizeof(*factory));
    kobj_header_init(&factory->hdr, CAP_OBJ_FACTORY);
    if (generation != 0U) {
        factory->hdr.generation = generation;
    }
    factory->allowed_mask = allowed_mask;
    factory->in_use = 1U;

    cap_id_t cap = cap_create_for_gen_badge(
        owner, factory, CAP_OBJ_FACTORY, rights, factory->hdr.generation,
        allowed_mask);
    /* cap_create_for_gen_badge queues the registered cleanup callback when
     * CSpace/cap-pool installation fails, so no second release is needed. */
    irq_spin_unlock(&factory_lock, crit);
    return cap;
}

static void factory_delete_created(uint8_t obj_type, int id) {
    switch (obj_type) {
        case CAP_OBJ_TASK:
            (void)task_delete((task_id_t)id);
            break;
        case CAP_OBJ_SEMAPHORE:
            (void)sem_delete((sem_id_t)id);
            break;
        case CAP_OBJ_MUTEX:
            (void)mutex_delete((mutex_id_t)id);
            break;
        case CAP_OBJ_MQUEUE:
            (void)mqueue_delete((queue_id_t)id);
            break;
        case CAP_OBJ_EVENT:
            (void)event_delete((event_id_t)id);
            break;
#if IPC_NOTIFICATION
        case CAP_OBJ_NOTIFICATION:
            (void)notification_delete((notification_id_t)id);
            break;
#endif
        case CAP_OBJ_TIMER:
            (void)timer_delete((timer_id_t)id);
            break;
        case CAP_OBJ_ENDPOINT:
            (void)endpoint_delete((ep_id_t)id);
            break;
        case CAP_OBJ_CHANNEL:
            (void)channel_delete((ch_id_t)id);
            break;
        default:
            break;
    }
}

static int factory_allocate_object(tcb_t *caller,
                                   const factory_create_request_t *request,
                                   void **out_object) {
    (void)caller; /* P2-2: TIMER 的 USER/entry 检查上移到入口(entry 一律拒) */
    int id;
    switch (request->obj_type) {
        case CAP_OBJ_TASK:
            if (request->entry == 0U) {
                return KERN_ERR_PARAM;
            }
            id = task_create_user(
                request->name[0] != '\0' ? request->name : "factory_task",
                (task_func_t)(uintptr_t)request->entry,
                (void *)(uintptr_t)request->arg,
                (uint8_t)request->param0, request->param1);
            *out_object = id >= 0 ? task_obj_for_cap((task_id_t)id) : NULL;
            break;
        case CAP_OBJ_SEMAPHORE:
            id = sem_create(request->param0, request->param1);
            *out_object = id >= 0 ? sem_obj_for_cap((sem_id_t)id) : NULL;
            break;
        case CAP_OBJ_MUTEX:
            id = mutex_create();
            *out_object = id >= 0 ? mutex_obj_for_cap((mutex_id_t)id) : NULL;
            break;
        case CAP_OBJ_MQUEUE:
            id = mqueue_create(request->param0, request->param1);
            *out_object = id >= 0 ? mqueue_obj_for_cap((queue_id_t)id) : NULL;
            break;
        case CAP_OBJ_EVENT:
            id = event_create(request->param0);
            *out_object = id >= 0 ? event_obj_for_cap((event_id_t)id) : NULL;
            break;
#if IPC_NOTIFICATION
        case CAP_OBJ_NOTIFICATION:
            id = notification_create();
            *out_object = id >= 0
                ? notification_obj_for_cap((notification_id_t)id) : NULL;
            break;
#endif
        case CAP_OBJ_TIMER:
            /* P2-2: 内核回调路径已删 —— entry 一律拒绝(保持字段兼容) */
            if (request->entry != 0U) {
                return KERN_ERR_PERM;
            }
            id = timer_create(
                request->name[0] != '\0' ? request->name : NULL,
                request->param0);
            *out_object = id >= 0 ? timer_obj_for_cap((timer_id_t)id) : NULL;
            break;
        case CAP_OBJ_ENDPOINT:
            id = endpoint_create(
                request->name[0] != '\0' ? request->name : NULL,
                (uint16_t)request->param0, (uint16_t)request->param1);
            *out_object = id >= 0 ? endpoint_obj_for_cap((ep_id_t)id) : NULL;
            break;
        case CAP_OBJ_CHANNEL:
            id = channel_create((uint16_t)request->param0, request->param1);
            *out_object = id >= 0 ? channel_obj_for_cap((ch_id_t)id) : NULL;
            break;
        default:
            return KERN_ERR_PARAM;
    }
    return id;
}

cap_id_t factory_create_for(tcb_t *caller, cap_id_t factory_cap,
                            const factory_create_request_t *request) {
    if (caller == NULL || request == NULL || request->flags != 0U ||
        request->obj_type >= CAP_OBJ_TYPE_MAX ||
        (request->rights & ~CAP_FULL) != 0U) {
        return KERN_ERR_PARAM;
    }

    void *object = NULL;
    uint32_t cap_mask = 0U;
    kern_err_t err = cap_object_pin_for(
        caller, factory_cap, CAP_OBJ_FACTORY, CAP_WRITE, &object, &cap_mask);
    if (err != KERN_OK) {
        return err;
    }

    factory_object_t *factory = (factory_object_t *)object;
    uint32_t requested_bit = FACTORY_OBJECT_BIT(request->obj_type);
    if (!factory->in_use ||
        (factory->allowed_mask & requested_bit) == 0U ||
        (cap_mask & requested_bit) == 0U) {
        (void)cap_object_unpin(factory, CAP_OBJ_FACTORY);
        return KERN_ERR_PERM;
    }

    uint8_t rights = request->rights != 0U ? request->rights : CAP_FULL;
    if (request->obj_type == CAP_OBJ_FRAME) {
        cap_id_t frame_cap =
            kframe_create_cap_for(caller, (size_t)request->param0, rights);
        (void)cap_object_unpin(factory, CAP_OBJ_FACTORY);
        return frame_cap;
    }

    void *created_object = NULL;
    int id = factory_allocate_object(caller, request, &created_object);
    if (id < 0 || created_object == NULL) {
        (void)cap_object_unpin(factory, CAP_OBJ_FACTORY);
        return id < 0 ? (cap_id_t)id : (cap_id_t)KERN_ERR_RESOURCE;
    }

    uint32_t generation =
        ((const kobject_header_t *)created_object)->generation;
    cap_id_t created_cap = cap_create_for_gen(
        caller, created_object, request->obj_type, rights, generation);
    if (created_cap < 0) {
        factory_delete_created(request->obj_type, id);
        created_cap = KERN_ERR_RESOURCE;
    }

    (void)cap_object_unpin(factory, CAP_OBJ_FACTORY);
    return created_cap;
}

#endif /* CAP_ENABLE */
