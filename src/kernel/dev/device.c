/**
 * @file device.c
 * @brief 设备注册表 — 静态池管理
 */

#include "device.h"
#include "trace.h"
#include "stats.h"
#if VFS_ENABLE
#include "devfs.h"
#endif
#include "hal.h"
#include "scheduler.h"
#include "spinlock.h"
#include <string.h>

#ifndef TRACE_DEV_OPEN
#define TRACE_DEV_OPEN        1
#define TRACE_DEV_READ        2
#define TRACE_DEV_WRITE       3
#define TRACE_DEV_IOCTL       4
#define TRACE_DEV_REMOVE      5
#endif

#ifndef STATS_COUNTER_OK
#define STATS_COUNTER_OK         0
#define STATS_COUNTER_ERROR      1
#define STATS_COUNTER_QUEUE_FULL 2
#define STATS_COUNTER_DELETE     4
#define STATS_COUNTER_BUSY       6
#define STATS_COUNTER_NOEXIST    7
#endif

#if DRIVER_ENABLE

/*============================================================================
 * 静态设备池
 *============================================================================*/

static device_t device_pool[DEVICE_MAX];
static irq_spinlock_t device_lock;

static uint8_t device_current_task_id(void) {
    tcb_t *current = sched_get_current();
    return current ? (uint8_t)current->id : 0xFFU;
}

int16_t device_get_id(device_t *dev) {
    if (!dev) return KERN_INVALID_ID;
    int idx = (int)(dev - device_pool);
    if (idx < 0 || idx >= DEVICE_MAX) return KERN_INVALID_ID;
    return (int16_t)idx;
}

#if TRACE_ENABLE
static uint8_t device_trace_result(kern_err_t err) {
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

static void device_record_event(device_t *dev, uint8_t action,
                                kern_err_t err, uint8_t counter) {
#if TRACE_ENABLE
    int16_t id = device_get_id(dev);
    uint8_t object_id = (id >= 0) ? (uint8_t)id : 0xFFU;
    trace_dev(device_current_task_id(), object_id, action,
              device_trace_result(err));
#else
    (void)dev;
    (void)action;
#endif

#if KERN_TASK_STATS
    if (err != KERN_OK) {
        if (err == KERN_ERR_BUSY) {
            counter = STATS_COUNTER_BUSY;
        } else if (err == KERN_ERR_NOEXIST) {
            counter = STATS_COUNTER_NOEXIST;
        } else {
            counter = STATS_COUNTER_ERROR;
        }
    }
    (void)stats_record_event(STATS_SUBSYS_DEV, counter);
#else
    (void)counter;
#endif
    (void)err;
}

/*============================================================================
 * API
 *============================================================================*/

void device_init(void) {
    irq_spin_init_rank(&device_lock, LOCKDEP_RANK_REGISTRY);
    memset(device_pool, 0, sizeof(device_pool));
}

device_t *device_alloc(const char *name, device_type_t type) {
    if (!name) {
        device_record_event(NULL, TRACE_DEV_OPEN, KERN_ERR_PARAM,
                            STATS_COUNTER_ERROR);
        return NULL;
    }

    uint32_t crit = irq_spin_lock(&device_lock);
    for (int i = 0; i < DEVICE_MAX; i++) {
        if (!device_pool[i].in_use) {
            device_t *dev = &device_pool[i];
            memset(dev, 0, sizeof(device_t));
            strncpy(dev->name, name, DEVICE_NAME_LEN - 1);
            dev->name[DEVICE_NAME_LEN - 1] = '\0';
            dev->type = type;
            dev->in_use = 1;
            irq_spin_unlock(&device_lock, crit);
            return dev;
        }
    }
    irq_spin_unlock(&device_lock, crit);
    device_record_event(NULL, TRACE_DEV_OPEN, KERN_ERR_RESOURCE,
                        STATS_COUNTER_QUEUE_FULL);
    return NULL;  /* 池满 */
}

void device_free(device_t *dev) {
    if (!dev) return;
    uint32_t crit = irq_spin_lock(&device_lock);
    if (!dev->in_use || dev->open_count > 0) {
        irq_spin_unlock(&device_lock, crit);
        return;
    }
    memset(dev, 0, sizeof(device_t));
    irq_spin_unlock(&device_lock, crit);
    device_record_event(dev, TRACE_DEV_REMOVE, KERN_OK, STATS_COUNTER_DELETE);
}

device_t *device_find(const char *name) {
    if (!name) return NULL;

    uint32_t crit = irq_spin_lock(&device_lock);
    for (int i = 0; i < DEVICE_MAX; i++) {
        if (device_pool[i].in_use &&
            strcmp(device_pool[i].name, name) == 0) {
            irq_spin_unlock(&device_lock, crit);
            return &device_pool[i];
        }
    }
    irq_spin_unlock(&device_lock, crit);
    return NULL;
}

device_t *device_get_by_index(uint16_t index) {
    if (index >= DEVICE_MAX) return NULL;
    uint32_t crit = irq_spin_lock(&device_lock);
    device_t *dev = device_pool[index].in_use ? &device_pool[index] : NULL;
    irq_spin_unlock(&device_lock, crit);
    return dev;
}

kern_err_t device_probe(const char *name, device_type_t type, dev_ops_t *ops,
                        void *priv, uint32_t irq_num) {
    if (!name || !ops) return KERN_ERR_PARAM;
    device_t *dev = NULL;
    uint32_t crit = irq_spin_lock(&device_lock);
    for (int i = 0; i < DEVICE_MAX; i++) {
        if (device_pool[i].in_use &&
            strcmp(device_pool[i].name, name) == 0) {
            irq_spin_unlock(&device_lock, crit);
            return KERN_ERR_BUSY;
        }
        if (dev == NULL && !device_pool[i].in_use) {
            dev = &device_pool[i];
        }
    }
    if (dev == NULL) {
        irq_spin_unlock(&device_lock, crit);
        return KERN_ERR_RESOURCE;
    }
    memset(dev, 0, sizeof(*dev));
    strncpy(dev->name, name, DEVICE_NAME_LEN - 1);
    dev->name[DEVICE_NAME_LEN - 1] = '\0';
    dev->type = type;
    dev->ops = ops;
    dev->priv = priv;
    dev->irq_num = irq_num;
    dev->events = 0;
    dev->in_use = 1;
    irq_spin_unlock(&device_lock, crit);

#if VFS_ENABLE
    kern_err_t err = devfs_register_device(name, dev);
    if (err != KERN_OK) {
        device_record_event(dev, TRACE_DEV_OPEN, err, STATS_COUNTER_ERROR);
        device_free(dev);
        return err;
    }
#endif

    device_record_event(dev, TRACE_DEV_OPEN, KERN_OK, STATS_COUNTER_OK);
    return KERN_OK;
}

kern_err_t device_remove(const char *name) {
    if (!name) return KERN_ERR_PARAM;

    device_t *dev = device_find(name);
    if (!dev) {
        device_record_event(NULL, TRACE_DEV_REMOVE, KERN_ERR_NOEXIST,
                            STATS_COUNTER_NOEXIST);
        return KERN_ERR_NOEXIST;
    }
    if (dev->open_count > 0) {
        device_record_event(dev, TRACE_DEV_REMOVE, KERN_ERR_BUSY,
                            STATS_COUNTER_BUSY);
        return KERN_ERR_BUSY;
    }

    (void)device_notify_events(dev, DEVICE_EVENT_REMOVED);

#if VFS_ENABLE
    kern_err_t err = devfs_unregister_device(name);
    if (err != KERN_OK) {
        device_record_event(dev, TRACE_DEV_REMOVE, err, STATS_COUNTER_ERROR);
        return err;
    }
#endif

    device_free(dev);
    return KERN_OK;
}

kern_err_t device_notify_events(device_t *dev, uint32_t events) {
    if (!dev) return KERN_ERR_NOEXIST;

    uint32_t crit = irq_spin_lock(&device_lock);
    if (!dev->in_use) {
        irq_spin_unlock(&device_lock, crit);
        return KERN_ERR_NOEXIST;
    }
    dev->events |= events;
    irq_spin_unlock(&device_lock, crit);
    return KERN_OK;
}

kern_err_t device_clear_events(device_t *dev, uint32_t events) {
    if (!dev) return KERN_ERR_NOEXIST;

    uint32_t crit = irq_spin_lock(&device_lock);
    if (!dev->in_use) {
        irq_spin_unlock(&device_lock, crit);
        return KERN_ERR_NOEXIST;
    }
    dev->events &= ~events;
    irq_spin_unlock(&device_lock, crit);
    return KERN_OK;
}

uint32_t device_get_events(device_t *dev) {
    if (!dev) return 0;

    uint32_t crit = irq_spin_lock(&device_lock);
    uint32_t events = dev->in_use ? dev->events : 0U;
    irq_spin_unlock(&device_lock, crit);
    return events;
}

#endif /* DRIVER_ENABLE */
