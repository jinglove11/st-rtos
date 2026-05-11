/**
 * @file syscall.c
 * @brief 系统调用分发表 — 统一 6-arg 签名 + 能力系统集成
 */

#include "syscall.h"
#include "task.h"
#include "scheduler.h"
#include "semaphore.h"
#include "mutex.h"
#include "mqueue.h"
#include "event.h"
#include "endpoint.h"
#include "channel.h"
#include "timer.h"
#include "irq.h"
#include "bh.h"
#include "mem.h"
#include "mempool.h"
#include "kernel_config.h"
#include "mpu.h"

#if CAP_ENABLE
#include "capability.h"
#endif

#if TRACE_ENABLE
#include "trace.h"
#include "stats.h"
#endif

#if SYSCALL_ENABLE

#define U(a) (void)a
#define U6 U(a1);U(a2);U(a3);U(a4);U(a5);U(a6)
#define U5 U(a1);U(a2);U(a3);U(a4);U(a5)
#define U4 U(a1);U(a2);U(a3);U(a4)
#define U3 U(a1);U(a2);U(a3)
#define U2 U(a1);U(a2)
#define U1 U(a1)

#define SYSCALL_FLASH_BASE 0x08000000UL
#define SYSCALL_FLASH_SIZE (2UL * 1024 * 1024)
#define SYSCALL_SRAM_BASE  0x20000000UL
#define SYSCALL_SRAM_SIZE  (384UL * 1024)
#define SYSCALL_PATH_MAX   128
#define MPU_REGION_MAX     8
#define MPU_AP_MASK        (0x7UL << 24)

static int ptr_in_region(uintptr_t ptr, uint32_t len,
                         uintptr_t base, uint32_t size) {
    if (len == 0) {
        return 1;
    }

    if (ptr < base) {
        return 0;
    }

    uintptr_t end = ptr + len - 1;
    if (end < ptr) {
        return 0;
    }

    return end < (base + size);
}

static int user_ap_allows_read(uint32_t rasr) {
    uint32_t ap = rasr & MPU_AP_MASK;
    return ap == AP_PRW_URO || ap == AP_FULL;
}

static int user_ap_allows_write(uint32_t rasr) {
    uint32_t ap = rasr & MPU_AP_MASK;
    return ap == AP_FULL;
}

static uint32_t mpu_region_size(uint32_t rasr) {
    uint32_t size_field = (rasr >> 1) & 0x1F;
    if (size_field < 4) {
        return 0;
    }
    return 1UL << (size_field + 1);
}

static int mpu_subregions_allow(uintptr_t start, uintptr_t end,
                                uintptr_t base, uint32_t size,
                                uint32_t rasr) {
    uint32_t srd = (rasr >> RASR_SRD_SHIFT) & 0xFF;
    if (srd == 0 || size < 256) {
        return 1;
    }

    uint32_t sub_size = size / 8;
    for (uint32_t sub = 0; sub < 8; sub++) {
        if ((srd & (1U << sub)) == 0) {
            continue;
        }

        uintptr_t sub_start = base + (sub * sub_size);
        uintptr_t sub_end = sub_start + sub_size - 1;
        if (start <= sub_end && end >= sub_start) {
            return 0;
        }
    }

    return 1;
}

static int user_range_allowed_by_mpu(const void *ptr, uint32_t len,
                                     int require_write) {
#if MPU_ENABLE
    tcb_t *cur = sched_get_current();
    if (cur == NULL || (cur->attrs & TASK_ATTR_USER) == 0) {
        return 0;
    }

    uintptr_t p = (uintptr_t)ptr;
    if (p == 0 && len > 0) {
        return 0;
    }
    if (len == 0) {
        return 1;
    }

    uintptr_t end = p + len - 1;
    if (end < p) {
        return 0;
    }

    for (uint32_t i = 0; i < MPU_REGION_MAX; i++) {
        uint32_t rbar = cur->mpu_regions[i][0];
        uint32_t rasr = cur->mpu_regions[i][1];
        if ((rasr & RASR_ENABLE) == 0) {
            continue;
        }
        if (require_write) {
            if (!user_ap_allows_write(rasr)) {
                continue;
            }
        } else if (!user_ap_allows_read(rasr)) {
            continue;
        }

        uint32_t size = mpu_region_size(rasr);
        uintptr_t base = rbar & ~(uintptr_t)0x1F;
        if (ptr_in_region(p, len, base, size) &&
            mpu_subregions_allow(p, end, base, size, rasr)) {
            return 1;
        }
    }
#else
    (void)ptr;
    (void)len;
    (void)require_write;
#endif
    return 0;
}

static int user_readable(const void *ptr, uint32_t len) {
    uintptr_t p = (uintptr_t)ptr;

    if (p == 0 && len > 0) {
        return 0;
    }

    if (user_range_allowed_by_mpu(ptr, len, 0)) {
        return 1;
    }

    tcb_t *cur = sched_get_current();
    if (cur && (cur->attrs & TASK_ATTR_USER)) {
        return 0;
    }

    return ptr_in_region(p, len, SYSCALL_FLASH_BASE, SYSCALL_FLASH_SIZE) ||
           ptr_in_region(p, len, SYSCALL_SRAM_BASE, SYSCALL_SRAM_SIZE);
}

static int user_writable(void *ptr, uint32_t len) {
    uintptr_t p = (uintptr_t)ptr;

    if (p == 0 && len > 0) {
        return 0;
    }

    if (user_range_allowed_by_mpu(ptr, len, 1)) {
        return 1;
    }

    tcb_t *cur = sched_get_current();
    if (cur && (cur->attrs & TASK_ATTR_USER)) {
        return 0;
    }

    return ptr_in_region(p, len, SYSCALL_SRAM_BASE, SYSCALL_SRAM_SIZE);
}

static kern_err_t user_copy_string(char *dst, uint32_t dst_size,
                                   const char *src) {
    if (dst == NULL || dst_size == 0) {
        return KERN_ERR_PARAM;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return KERN_OK;
    }

    for (uint32_t i = 0; i < dst_size; i++) {
        if (!user_readable(src + i, 1)) {
            dst[0] = '\0';
            return KERN_ERR_PARAM;
        }

        dst[i] = src[i];
        if (dst[i] == '\0') {
            return KERN_OK;
        }
    }

    dst[dst_size - 1] = '\0';
    return KERN_ERR_PARAM;
}

/*============================================================================
 * 任务管理
 *============================================================================*/

static int sys_task_yield(uint32_t a1, uint32_t a2, uint32_t a3,
                                  uint32_t a4, uint32_t a5, uint32_t a6) {
    U6; sched_yield(); return KERN_OK;
}

static int sys_task_delay(uint32_t a1, uint32_t a2, uint32_t a3,
                                  uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
    return task_delay(a1);
}

static int sys_task_exit(uint32_t a1, uint32_t a2, uint32_t a3,
                                 uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
    return task_exit_request((void *)(uintptr_t)a1);
}

static int sys_task_create(uint32_t a1, uint32_t a2, uint32_t a3,
                                   uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a6);
    char name_buf[KERN_TASK_NAME_LEN];
    task_func_t entry = (task_func_t)(uintptr_t)a2;
    void *arg         = (void *)(uintptr_t)a3;
    uint8_t priority  = (uint8_t)a4;
    uint32_t stack_sz = a5;

    if (!user_readable((const void *)(uintptr_t)a2, 1)) {
        return KERN_ERR_PARAM;
    }

    kern_err_t copy_err = user_copy_string(name_buf, sizeof(name_buf),
                                           (const char *)(uintptr_t)a1);
    if (copy_err != KERN_OK) {
        return copy_err;
    }

#if MPU_ENABLE
    task_id_t id = task_create_user(name_buf[0] ? name_buf : NULL,
                                    entry, arg, priority, stack_sz);
#else
    task_id_t id = task_create(name_buf[0] ? name_buf : NULL,
                               entry, arg, priority, stack_sz);
#endif
    if (id < 0) return id;

#if CAP_ENABLE
    tcb_t *cur = sched_get_current();
    cap_id_t cap = cap_create((void *)(uintptr_t)(id + 1), CAP_OBJ_TASK,
                              CAP_FULL, (uint8_t)(cur ? cur->id : 0));
    if (cap < 0) { task_delete(id); return KERN_ERR_RESOURCE; }
    return (int)cap;
#else
    return (int)id;
#endif
}

static int sys_task_start(uint32_t a1, uint32_t a2, uint32_t a3,
                                  uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_TASK, CAP_MANAGE);
    if (!obj) return KERN_ERR_CAP;
    return task_start((task_id_t)((uintptr_t)obj - 1));
#else
    return task_start((task_id_t)a1);
#endif
}

static int sys_task_suspend(uint32_t a1, uint32_t a2, uint32_t a3,
                                    uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
    return task_suspend((task_id_t)a1);
}

static int sys_task_resume(uint32_t a1, uint32_t a2, uint32_t a3,
                                   uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
    return task_resume((task_id_t)a1);
}

static int sys_task_delete(uint32_t a1, uint32_t a2, uint32_t a3,
                                   uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
    return task_delete((task_id_t)a1);
}

static int sys_task_self(uint32_t a1, uint32_t a2, uint32_t a3,
                                 uint32_t a4, uint32_t a5, uint32_t a6) {
    U6; return (kern_err_t)task_self();
}

/*============================================================================
 * 信号量
 *============================================================================*/

static int sys_sem_create(uint32_t a1, uint32_t a2, uint32_t a3,
                                  uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    sem_id_t id = sem_create(a2, a1);
    if (id < 0) return id;
    tcb_t *cur = sched_get_current();
    cap_id_t cap = cap_create((void *)(uintptr_t)(id + 1), CAP_OBJ_SEMAPHORE,
                              CAP_FULL, (uint8_t)(cur ? cur->id : 0));
    if (cap < 0) { sem_delete(id); return KERN_ERR_RESOURCE; }
    return (int)cap;
#else
    return (kern_err_t)sem_create(a2, a1);
#endif
}

static int sys_sem_wait(uint32_t a1, uint32_t a2, uint32_t a3,
                                uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_SEMAPHORE, CAP_READ);
    if (!obj) return KERN_ERR_CAP;
    return sem_wait((sem_id_t)((uintptr_t)obj - 1), a2);
#else
    return sem_wait((sem_id_t)a1, a2);
#endif
}

static int sys_sem_post(uint32_t a1, uint32_t a2, uint32_t a3,
                                uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_SEMAPHORE, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;
    return sem_post((sem_id_t)((uintptr_t)obj - 1));
#else
    return sem_post((sem_id_t)a1);
#endif
}

static int sys_sem_delete(uint32_t a1, uint32_t a2, uint32_t a3,
                                  uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_SEMAPHORE, CAP_MANAGE);
    if (!obj) return KERN_ERR_CAP;
    sem_id_t id = (sem_id_t)((uintptr_t)obj - 1);
    kern_err_t ret = sem_delete(id);
    cap_delete((cap_id_t)a1);
    return ret;
#else
    return sem_delete((sem_id_t)a1);
#endif
}

/*============================================================================
 * 互斥锁
 *============================================================================*/

static int sys_mutex_create(uint32_t a1, uint32_t a2, uint32_t a3,
                                    uint32_t a4, uint32_t a5, uint32_t a6) {
    U6;
#if CAP_ENABLE
    mutex_id_t id = mutex_create();
    if (id < 0) return id;
    tcb_t *cur = sched_get_current();
    cap_id_t cap = cap_create((void *)(uintptr_t)(id + 1), CAP_OBJ_MUTEX,
                              CAP_FULL, (uint8_t)(cur ? cur->id : 0));
    if (cap < 0) { /* mutex_delete(id); */ return KERN_ERR_RESOURCE; }
    return (int)cap;
#else
    return (kern_err_t)mutex_create();
#endif
}

static int sys_mutex_lock(uint32_t a1, uint32_t a2, uint32_t a3,
                                  uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_MUTEX, CAP_READ);
    if (!obj) return KERN_ERR_CAP;
    return mutex_lock((mutex_id_t)((uintptr_t)obj - 1), a2);
#else
    return mutex_lock((mutex_id_t)a1, a2);
#endif
}

static int sys_mutex_unlock(uint32_t a1, uint32_t a2, uint32_t a3,
                                    uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_MUTEX, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;
    return mutex_unlock((mutex_id_t)((uintptr_t)obj - 1));
#else
    return mutex_unlock((mutex_id_t)a1);
#endif
}

/*============================================================================
 * 消息队列
 *============================================================================*/

static int sys_mqueue_create(uint32_t a1, uint32_t a2, uint32_t a3,
                                     uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    queue_id_t id = mqueue_create(a1, a2);
    if (id < 0) return id;
    tcb_t *cur = sched_get_current();
    cap_id_t cap = cap_create((void *)(uintptr_t)(id + 1), CAP_OBJ_MQUEUE,
                              CAP_FULL, (uint8_t)(cur ? cur->id : 0));
    if (cap < 0) { /* mqueue_delete(id); */ return KERN_ERR_RESOURCE; }
    return (int)cap;
#else
    return (kern_err_t)mqueue_create(a1, a2);
#endif
}

static int sys_mqueue_send(uint32_t a1, uint32_t a2, uint32_t a3,
                                   uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
    if (!user_readable((const void *)(uintptr_t)a2, KERN_MSG_MAX_SIZE)) {
        return KERN_ERR_PARAM;
    }
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_MQUEUE, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;
    return mqueue_send((queue_id_t)((uintptr_t)obj - 1), (const void *)a2, a3);
#else
    return mqueue_send((queue_id_t)a1, (const void *)a2, a3);
#endif
}

static int sys_mqueue_recv(uint32_t a1, uint32_t a2, uint32_t a3,
                                   uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
    if (!user_writable((void *)(uintptr_t)a2, KERN_MSG_MAX_SIZE)) {
        return KERN_ERR_PARAM;
    }
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_MQUEUE, CAP_READ);
    if (!obj) return KERN_ERR_CAP;
    return mqueue_recv((queue_id_t)((uintptr_t)obj - 1), (void *)a2, a3);
#else
    return mqueue_recv((queue_id_t)a1, (void *)a2, a3);
#endif
}

/*============================================================================
 * 事件
 *============================================================================*/

static int sys_event_create(uint32_t a1, uint32_t a2, uint32_t a3,
                                    uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    event_id_t id = event_create(a1);
    if (id < 0) return id;
    tcb_t *cur = sched_get_current();
    cap_id_t cap = cap_create((void *)(uintptr_t)(id + 1), CAP_OBJ_EVENT,
                              CAP_FULL, (uint8_t)(cur ? cur->id : 0));
    if (cap < 0) { /* event_delete(id); */ return KERN_ERR_RESOURCE; }
    return (int)cap;
#else
    return (kern_err_t)event_create(a1);
#endif
}

static int sys_event_wait(uint32_t a1, uint32_t a2, uint32_t a3,
                                  uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_EVENT, CAP_READ);
    if (!obj) return KERN_ERR_CAP;
    return event_wait((event_id_t)((uintptr_t)obj - 1), a2, 0, a3, NULL);
#else
    return event_wait((event_id_t)a1, a2, 0, a3, NULL);
#endif
}

static int sys_event_set(uint32_t a1, uint32_t a2, uint32_t a3,
                                 uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_EVENT, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;
    return event_set((event_id_t)((uintptr_t)obj - 1), a2);
#else
    return event_set((event_id_t)a1, a2);
#endif
}

static int sys_mutex_delete(uint32_t a1, uint32_t a2, uint32_t a3,
                                    uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_MUTEX, CAP_MANAGE);
    if (!obj) return KERN_ERR_CAP;
    mutex_id_t id = (mutex_id_t)((uintptr_t)obj - 1);
    kern_err_t ret = mutex_delete(id);
    cap_delete((cap_id_t)a1);
    return ret;
#else
    return mutex_delete((mutex_id_t)a1);
#endif
}

static int sys_mqueue_delete(uint32_t a1, uint32_t a2, uint32_t a3,
                                     uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_MQUEUE, CAP_MANAGE);
    if (!obj) return KERN_ERR_CAP;
    queue_id_t id = (queue_id_t)((uintptr_t)obj - 1);
    kern_err_t ret = mqueue_delete(id);
    cap_delete((cap_id_t)a1);
    return ret;
#else
    return mqueue_delete((queue_id_t)a1);
#endif
}

static int sys_event_delete(uint32_t a1, uint32_t a2, uint32_t a3,
                                    uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_EVENT, CAP_MANAGE);
    if (!obj) return KERN_ERR_CAP;
    event_id_t id = (event_id_t)((uintptr_t)obj - 1);
    kern_err_t ret = event_delete(id);
    cap_delete((cap_id_t)a1);
    return ret;
#else
    return event_delete((event_id_t)a1);
#endif
}

static int sys_event_clear(uint32_t a1, uint32_t a2, uint32_t a3,
                                   uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_EVENT, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;
    return event_clear((event_id_t)((uintptr_t)obj - 1), a2);
#else
    return event_clear((event_id_t)a1, a2);
#endif
}

static int sys_event_get(uint32_t a1, uint32_t a2, uint32_t a3,
                                 uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_EVENT, CAP_READ);
    if (!obj) return KERN_ERR_CAP;
    return (int)event_get((event_id_t)((uintptr_t)obj - 1));
#else
    return (int)event_get((event_id_t)a1);
#endif
}

/*============================================================================
 * Endpoint
 *============================================================================*/

static int sys_ep_create(uint32_t a1, uint32_t a2, uint32_t a3,
                                 uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
    char name_buf[ENDPOINT_NAME_LEN];
    kern_err_t copy_err = user_copy_string(name_buf, sizeof(name_buf),
                                           (const char *)(uintptr_t)a1);
    if (copy_err != KERN_OK) {
        return copy_err;
    }

#if CAP_ENABLE
    ep_id_t id = endpoint_create(name_buf[0] ? name_buf : NULL,
                                 (uint16_t)a2, (uint16_t)a3);
    if (id < 0) return id;
    tcb_t *cur = sched_get_current();
    cap_id_t cap = cap_create((void *)(uintptr_t)(id + 1), CAP_OBJ_ENDPOINT,
                              CAP_FULL, (uint8_t)(cur ? cur->id : 0));
    if (cap < 0) { endpoint_delete(id); return KERN_ERR_RESOURCE; }
    return (int)cap;
#else
    return (int)endpoint_create(name_buf[0] ? name_buf : NULL,
                                (uint16_t)a2, (uint16_t)a3);
#endif
}

static int sys_ep_delete(uint32_t a1, uint32_t a2, uint32_t a3,
                                 uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_ENDPOINT, CAP_MANAGE);
    if (!obj) return KERN_ERR_CAP;
    ep_id_t id = (ep_id_t)((uintptr_t)obj - 1);
    kern_err_t ret = endpoint_delete(id);
    cap_delete((cap_id_t)a1);
    return ret;
#else
    return endpoint_delete((ep_id_t)a1);
#endif
}

static int sys_ep_send(uint32_t a1, uint32_t a2, uint32_t a3,
                               uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
    if (!user_readable((const void *)(uintptr_t)a2, KERN_EP_MSG_SIZE)) {
        return KERN_ERR_PARAM;
    }
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_ENDPOINT, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;
    return endpoint_send((ep_id_t)((uintptr_t)obj - 1), (void *)a2, a3);
#else
    return endpoint_send((ep_id_t)a1, (void *)a2, a3);
#endif
}

static int sys_ep_send_caps(uint32_t a1, uint32_t a2, uint32_t a3,
                                    uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a6);
    uint8_t cap_count = (uint8_t)a4;
    if (cap_count > IPC_CAPS_MAX) {
        return KERN_ERR_PARAM;
    }
    if (!user_readable((const void *)(uintptr_t)a2, KERN_EP_MSG_SIZE)) {
        return KERN_ERR_PARAM;
    }
    if (cap_count > 0 &&
        !user_readable((const void *)(uintptr_t)a3,
                       sizeof(ipc_cap_xfer_t) * cap_count)) {
        return KERN_ERR_PARAM;
    }
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_ENDPOINT, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;
    return endpoint_send_caps((ep_id_t)((uintptr_t)obj - 1),
                              (void *)a2,
                              (const ipc_cap_xfer_t *)(uintptr_t)a3,
                              cap_count, a5);
#else
    return endpoint_send_caps((ep_id_t)a1, (void *)a2,
                              (const ipc_cap_xfer_t *)(uintptr_t)a3,
                              cap_count, a5);
#endif
}

static int sys_ep_recv(uint32_t a1, uint32_t a2, uint32_t a3,
                               uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
    if (!user_writable((void *)(uintptr_t)a2, KERN_EP_MSG_SIZE)) {
        return KERN_ERR_PARAM;
    }
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_ENDPOINT, CAP_READ);
    if (!obj) return KERN_ERR_CAP;
    return endpoint_recv((ep_id_t)((uintptr_t)obj - 1), (void *)a2, a3);
#else
    return endpoint_recv((ep_id_t)a1, (void *)a2, a3);
#endif
}

static int sys_ep_recv_caps(uint32_t a1, uint32_t a2, uint32_t a3,
                                    uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a6);
    if (!user_writable((void *)(uintptr_t)a2, KERN_EP_MSG_SIZE)) {
        return KERN_ERR_PARAM;
    }
    if (!user_writable((void *)(uintptr_t)a3,
                       sizeof(cap_id_t) * IPC_CAPS_MAX)) {
        return KERN_ERR_PARAM;
    }
    if (!user_writable((void *)(uintptr_t)a4, sizeof(uint8_t))) {
        return KERN_ERR_PARAM;
    }
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_ENDPOINT, CAP_READ);
    if (!obj) return KERN_ERR_CAP;
    return endpoint_recv_caps((ep_id_t)((uintptr_t)obj - 1), (void *)a2,
                              (cap_id_t *)(uintptr_t)a3,
                              (uint8_t *)(uintptr_t)a4, a5);
#else
    return endpoint_recv_caps((ep_id_t)a1, (void *)a2,
                              (cap_id_t *)(uintptr_t)a3,
                              (uint8_t *)(uintptr_t)a4, a5);
#endif
}

static int sys_ep_reply(uint32_t a1, uint32_t a2, uint32_t a3,
                                uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a3);U(a4);U(a5);U(a6);
    if (!user_readable((const void *)(uintptr_t)a2, KERN_EP_MSG_SIZE)) {
        return KERN_ERR_PARAM;
    }
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_ENDPOINT, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;
    return endpoint_reply((ep_id_t)((uintptr_t)obj - 1), (const void *)a2);
#else
    return endpoint_reply((ep_id_t)a1, (const void *)a2);
#endif
}

/*============================================================================
 * Channel
 *============================================================================*/

static int sys_ch_create(uint32_t a1, uint32_t a2, uint32_t a3,
                                 uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    ch_id_t id = channel_create((uint16_t)a1, a2);
    if (id < 0) return id;
    tcb_t *cur = sched_get_current();
    cap_id_t cap = cap_create((void *)(uintptr_t)(id + 1), CAP_OBJ_CHANNEL,
                              CAP_FULL, (uint8_t)(cur ? cur->id : 0));
    if (cap < 0) { channel_delete(id); return KERN_ERR_RESOURCE; }
    return (int)cap;
#else
    return (int)channel_create((uint16_t)a1, a2);
#endif
}

static int sys_ch_delete(uint32_t a1, uint32_t a2, uint32_t a3,
                                 uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_CHANNEL, CAP_MANAGE);
    if (!obj) return KERN_ERR_CAP;
    ch_id_t id = (ch_id_t)((uintptr_t)obj - 1);
    kern_err_t ret = channel_delete(id);
    cap_delete((cap_id_t)a1);
    return ret;
#else
    return channel_delete((ch_id_t)a1);
#endif
}

static int sys_ch_connect(uint32_t a1, uint32_t a2, uint32_t a3,
                                  uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_CHANNEL, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;
    return channel_connect((ch_id_t)((uintptr_t)obj - 1), (task_id_t)a2, (task_id_t)a3);
#else
    return channel_connect((ch_id_t)a1, (task_id_t)a2, (task_id_t)a3);
#endif
}

static int sys_ch_send(uint32_t a1, uint32_t a2, uint32_t a3,
                               uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
    if (!user_readable((const void *)(uintptr_t)a2, KERN_CH_MSG_SIZE)) {
        return KERN_ERR_PARAM;
    }
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_CHANNEL, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;
    return channel_send((ch_id_t)((uintptr_t)obj - 1), (const void *)a2, a3);
#else
    return channel_send((ch_id_t)a1, (const void *)a2, a3);
#endif
}

static int sys_ch_send_caps(uint32_t a1, uint32_t a2, uint32_t a3,
                                    uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a6);
    uint8_t cap_count = (uint8_t)a4;
    if (cap_count > IPC_CAPS_MAX) {
        return KERN_ERR_PARAM;
    }
    if (!user_readable((const void *)(uintptr_t)a2, KERN_CH_MSG_SIZE)) {
        return KERN_ERR_PARAM;
    }
    if (cap_count > 0 &&
        !user_readable((const void *)(uintptr_t)a3,
                       sizeof(ipc_cap_xfer_t) * cap_count)) {
        return KERN_ERR_PARAM;
    }
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_CHANNEL, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;
    return channel_send_caps((ch_id_t)((uintptr_t)obj - 1),
                             (const void *)a2,
                             (const ipc_cap_xfer_t *)(uintptr_t)a3,
                             cap_count, a5);
#else
    return channel_send_caps((ch_id_t)a1, (const void *)a2,
                             (const ipc_cap_xfer_t *)(uintptr_t)a3,
                             cap_count, a5);
#endif
}

static int sys_ch_recv(uint32_t a1, uint32_t a2, uint32_t a3,
                               uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
    if (!user_writable((void *)(uintptr_t)a2, KERN_CH_MSG_SIZE)) {
        return KERN_ERR_PARAM;
    }
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_CHANNEL, CAP_READ);
    if (!obj) return KERN_ERR_CAP;
    return channel_recv((ch_id_t)((uintptr_t)obj - 1), (void *)a2, a3);
#else
    return channel_recv((ch_id_t)a1, (void *)a2, a3);
#endif
}

static int sys_ch_recv_caps(uint32_t a1, uint32_t a2, uint32_t a3,
                                    uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a6);
    if (!user_writable((void *)(uintptr_t)a2, KERN_CH_MSG_SIZE)) {
        return KERN_ERR_PARAM;
    }
    if (!user_writable((void *)(uintptr_t)a3,
                       sizeof(cap_id_t) * IPC_CAPS_MAX)) {
        return KERN_ERR_PARAM;
    }
    if (!user_writable((void *)(uintptr_t)a4, sizeof(uint8_t))) {
        return KERN_ERR_PARAM;
    }
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_CHANNEL, CAP_READ);
    if (!obj) return KERN_ERR_CAP;
    return channel_recv_caps((ch_id_t)((uintptr_t)obj - 1), (void *)a2,
                             (cap_id_t *)(uintptr_t)a3,
                             (uint8_t *)(uintptr_t)a4, a5);
#else
    return channel_recv_caps((ch_id_t)a1, (void *)a2,
                             (cap_id_t *)(uintptr_t)a3,
                             (uint8_t *)(uintptr_t)a4, a5);
#endif
}

static int sys_ch_get_shm(uint32_t a1, uint32_t a2, uint32_t a3,
                                  uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_CHANNEL, CAP_READ);
    if (!obj) return KERN_ERR_CAP;
    void *shm = channel_get_shm((ch_id_t)((uintptr_t)obj - 1));
    return (int)(uintptr_t)shm;
#else
    void *shm = channel_get_shm((ch_id_t)a1);
    return (int)(uintptr_t)shm;
#endif
}

/*============================================================================
 * 定时器
 *============================================================================*/

static int sys_timer_create(uint32_t a1, uint32_t a2, uint32_t a3,
                                    uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a5);U(a6);
    char name_buf[TIMER_NAME_LEN];
    kern_err_t copy_err = user_copy_string(name_buf, sizeof(name_buf),
                                           (const char *)(uintptr_t)a1);
    if (copy_err != KERN_OK) {
        return copy_err;
    }

    if (!user_readable((const void *)(uintptr_t)a2, 1)) {
        return KERN_ERR_PARAM;
    }

#if CAP_ENABLE
    timer_id_t id = timer_create(name_buf[0] ? name_buf : NULL,
                                 (timer_callback_t)a2,
                                 (void *)a3, a4);
    if (id < 0) return id;
    tcb_t *cur = sched_get_current();
    cap_id_t cap = cap_create((void *)(uintptr_t)(id + 1), CAP_OBJ_TIMER,
                              CAP_FULL, (uint8_t)(cur ? cur->id : 0));
    if (cap < 0) return KERN_ERR_RESOURCE;
    return (int)cap;
#else
    return (kern_err_t)timer_create(name_buf[0] ? name_buf : NULL,
                                     (timer_callback_t)a2,
                                     (void *)a3, a4);
#endif
}

static int sys_timer_start(uint32_t a1, uint32_t a2, uint32_t a3,
                                   uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_TIMER, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;
    return timer_start((timer_id_t)((uintptr_t)obj - 1), a2);
#else
    return timer_start((timer_id_t)a1, a2);
#endif
}

/*============================================================================
 * 内存管理
 *============================================================================*/

static int sys_mem_alloc(uint32_t a1, uint32_t a2, uint32_t a3,
                                 uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
    if (a1 == 0) return KERN_ERR_PARAM;
    void *ptr = kmalloc(a1);
    if (!ptr) return KERN_ERR_RESOURCE;
#if CAP_ENABLE
    tcb_t *cur = sched_get_current();
    cap_id_t cap = cap_create(ptr, CAP_OBJ_MEMBLOCK,
                              CAP_READ | CAP_WRITE | CAP_MANAGE,
                              (uint8_t)(cur ? cur->id : 0));
    if (cap < 0) { kfree(ptr); return KERN_ERR_RESOURCE; }
    return (int)cap;
#else
    return (kern_err_t)(uintptr_t)ptr;
#endif
}

static int sys_mem_free(uint32_t a1, uint32_t a2, uint32_t a3,
                                uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *ptr = cap_resolve((cap_id_t)a1, CAP_OBJ_MEMBLOCK, CAP_MANAGE);
    if (!ptr) return KERN_ERR_CAP;
    kfree(ptr);
    cap_delete((cap_id_t)a1);
    return KERN_OK;
#else
    kfree((void *)a1);
    return KERN_OK;
#endif
}

/*============================================================================
 * 中断管理
 *============================================================================*/

static int sys_irq_register(uint32_t a1, uint32_t a2, uint32_t a3,
                                    uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
    return irq_register((int16_t)a1, (isr_func_t)a2, (uint8_t)a3);
}

static int sys_bh_create(uint32_t a1, uint32_t a2, uint32_t a3,
                                 uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a3);U(a4);U(a5);U(a6);
    return (kern_err_t)bh_create((bh_handler_t)a1, (void *)a2);
}

static int sys_bh_schedule(uint32_t a1, uint32_t a2, uint32_t a3,
                                   uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
    return bh_schedule((int16_t)a1);
}

/*============================================================================
 * 能力管理 (仅 CAP_ENABLE)
 *============================================================================*/

#if CAP_ENABLE

static int sys_cap_derive(uint32_t a1, uint32_t a2, uint32_t a3,
                                  uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a3);U(a4);U(a5);U(a6);
    cap_id_t result = cap_derive((cap_id_t)a1, (uint8_t)a2);
    return (result < 0) ? KERN_ERR_CAP : (kern_err_t)result;
}

static int sys_cap_transfer(uint32_t a1, uint32_t a2, uint32_t a3,
                                    uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a3);U(a4);U(a5);U(a6);
    return cap_transfer((cap_id_t)a1, (uint8_t)a2);
}

static int sys_cap_revoke(uint32_t a1, uint32_t a2, uint32_t a3,
                                  uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
    return cap_revoke((cap_id_t)a1);
}

#endif /* CAP_ENABLE */

/*============================================================================
 * VFS 文件操作 (VFS_ENABLE)
 *============================================================================*/

#if VFS_ENABLE
#include "vfs.h"

static int sys_open(uint32_t a1, uint32_t a2, uint32_t a3,
                            uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a3);U(a4);U(a5);U(a6);
    char path[SYSCALL_PATH_MAX];
    kern_err_t copy_err = user_copy_string(path, sizeof(path),
                                           (const char *)(uintptr_t)a1);
    if (copy_err != KERN_OK) {
        return copy_err;
    }
    return vfs_open(path, a2);
}

static int sys_close(uint32_t a1, uint32_t a2, uint32_t a3,
                             uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
    return (kern_err_t)vfs_close((int)a1);
}

static int sys_read(uint32_t a1, uint32_t a2, uint32_t a3,
                            uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
    if (!user_writable((void *)(uintptr_t)a2, a3)) {
        return KERN_ERR_PARAM;
    }
    return vfs_read((int)a1, (void *)a2, a3);
}

static int sys_write(uint32_t a1, uint32_t a2, uint32_t a3,
                             uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
    if (!user_readable((const void *)(uintptr_t)a2, a3)) {
        return KERN_ERR_PARAM;
    }
    return vfs_write((int)a1, (const void *)a2, a3);
}

static int sys_ioctl(uint32_t a1, uint32_t a2, uint32_t a3,
                             uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
    return (kern_err_t)vfs_ioctl((int)a1, a2, (void *)a3);
}

static int sys_lseek(uint32_t a1, uint32_t a2, uint32_t a3,
                             uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
    return vfs_lseek((int)a1, (int32_t)a2, (int)a3);
}

#endif /* VFS_ENABLE */

#undef U
#undef U1
#undef U2
#undef U3
#undef U4
#undef U5
#undef U6

/*============================================================================
 * 分发表
 *============================================================================*/

static const syscall_entry_t syscall_table[SYSCALL_TABLE_SIZE] = {
    SYSDEF(SYSCALL_TASK_YIELD,    sys_task_yield,    0),
    SYSDEF(SYSCALL_TASK_DELAY,    sys_task_delay,    1),
    SYSDEF(SYSCALL_TASK_EXIT,     sys_task_exit,     0),
    SYSDEF(SYSCALL_TASK_CREATE,   sys_task_create,   5),
    SYSDEF(SYSCALL_TASK_START,    sys_task_start,    1),
    SYSDEF(SYSCALL_TASK_SUSPEND,  sys_task_suspend,  1),
    SYSDEF(SYSCALL_TASK_RESUME,   sys_task_resume,   1),
    SYSDEF(SYSCALL_TASK_DELETE,   sys_task_delete,   1),
    SYSDEF(SYSCALL_TASK_SELF,     sys_task_self,     0),
    SYSDEF(SYSCALL_SEM_CREATE,    sys_sem_create,    2),
    SYSDEF(SYSCALL_SEM_WAIT,      sys_sem_wait,      2),
    SYSDEF(SYSCALL_SEM_POST,      sys_sem_post,      1),
    SYSDEF(SYSCALL_SEM_DELETE,    sys_sem_delete,    1),
    SYSDEF(SYSCALL_MUTEX_CREATE,  sys_mutex_create,  0),
    SYSDEF(SYSCALL_MUTEX_LOCK,    sys_mutex_lock,    2),
    SYSDEF(SYSCALL_MUTEX_UNLOCK,  sys_mutex_unlock,  1),
    SYSDEF(SYSCALL_MQUEUE_CREATE, sys_mqueue_create, 2),
    SYSDEF(SYSCALL_MQUEUE_SEND,   sys_mqueue_send,   3),
    SYSDEF(SYSCALL_MQUEUE_RECV,   sys_mqueue_recv,   3),
    SYSDEF(SYSCALL_EVENT_CREATE,  sys_event_create,  1),
    SYSDEF(SYSCALL_EVENT_WAIT,    sys_event_wait,    3),
    SYSDEF(SYSCALL_EVENT_SET,     sys_event_set,     2),
    SYSDEF(SYSCALL_MUTEX_DELETE,  sys_mutex_delete,  1),
    SYSDEF(SYSCALL_MQUEUE_DELETE, sys_mqueue_delete, 1),
    SYSDEF(SYSCALL_EVENT_DELETE,  sys_event_delete,  1),
    SYSDEF(SYSCALL_EVENT_CLEAR,   sys_event_clear,   2),
    SYSDEF(SYSCALL_EVENT_GET,     sys_event_get,     1),
    SYSDEF(SYSCALL_EP_CREATE,     sys_ep_create,     3),
    SYSDEF(SYSCALL_EP_DELETE,     sys_ep_delete,     1),
    SYSDEF(SYSCALL_EP_SEND,       sys_ep_send,       3),
    SYSDEF(SYSCALL_EP_RECV,       sys_ep_recv,       3),
    SYSDEF(SYSCALL_EP_REPLY,      sys_ep_reply,      2),
    SYSDEF(SYSCALL_EP_SEND_CAPS,  sys_ep_send_caps,  5),
    SYSDEF(SYSCALL_EP_RECV_CAPS,  sys_ep_recv_caps,  5),
    SYSDEF(SYSCALL_CH_CREATE,     sys_ch_create,     2),
    SYSDEF(SYSCALL_CH_DELETE,     sys_ch_delete,     1),
    SYSDEF(SYSCALL_CH_CONNECT,    sys_ch_connect,    3),
    SYSDEF(SYSCALL_CH_SEND,       sys_ch_send,       3),
    SYSDEF(SYSCALL_CH_RECV,       sys_ch_recv,       3),
    SYSDEF(SYSCALL_CH_GET_SHM,    sys_ch_get_shm,    1),
    SYSDEF(SYSCALL_CH_SEND_CAPS,  sys_ch_send_caps,  5),
    SYSDEF(SYSCALL_CH_RECV_CAPS,  sys_ch_recv_caps,  5),
    SYSDEF(SYSCALL_TIMER_CREATE,  sys_timer_create,  4),
    SYSDEF(SYSCALL_TIMER_START,   sys_timer_start,   2),
    SYSDEF(SYSCALL_IRQ_REGISTER,  sys_irq_register,  3),
    SYSDEF(SYSCALL_BH_CREATE,     sys_bh_create,     2),
    SYSDEF(SYSCALL_BH_SCHEDULE,   sys_bh_schedule,   1),
    SYSDEF(SYSCALL_MEM_ALLOC,     sys_mem_alloc,     1),
    SYSDEF(SYSCALL_MEM_FREE,      sys_mem_free,      1),
#if CAP_ENABLE
    SYSDEF(SYSCALL_CAP_DERIVE,    sys_cap_derive,    2),
    SYSDEF(SYSCALL_CAP_TRANSFER,  sys_cap_transfer,  2),
    SYSDEF(SYSCALL_CAP_REVOKE,    sys_cap_revoke,    1),
#endif
#if VFS_ENABLE
    SYSDEF(SYSCALL_OPEN,          sys_open,          2),
    SYSDEF(SYSCALL_CLOSE,         sys_close,         1),
    SYSDEF(SYSCALL_READ,          sys_read,          3),
    SYSDEF(SYSCALL_WRITE,         sys_write,         3),
    SYSDEF(SYSCALL_IOCTL,         sys_ioctl,         3),
    SYSDEF(SYSCALL_LSEEK,         sys_lseek,         3),
#endif
};

/*============================================================================
 * 分发入口
 *============================================================================*/

int kern_syscall_handler(uint32_t syscall_num,
                                 uint32_t a1, uint32_t a2, uint32_t a3,
                                 uint32_t a4, uint32_t a5, uint32_t a6) {
    if (syscall_num >= SYSCALL_TABLE_SIZE)
        return KERN_ERR_PARAM;

    tcb_t *cur = sched_get_current();
#if TRACE_ENABLE
    trace_record(TRACE_SYSCALL, (uint8_t)(cur ? cur->id : 0), (uint16_t)syscall_num);
#endif
#if KERN_TASK_STATS
    stats_record_syscall((uint8_t)(cur ? cur->id : 0));
#endif

    const syscall_entry_t *entry = &syscall_table[syscall_num];
    if (entry->handler == NULL)
        return KERN_ERR_PARAM;

    return entry->handler(a1, a2, a3, a4, a5, a6);
}

#endif /* SYSCALL_ENABLE */
