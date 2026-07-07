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
#if FAULT_ENDPOINT
#include "fault_endpoint.h"
#endif
#include "channel.h"
#include "timer.h"
#include "irq.h"
#include "bh.h"
#include "mem.h"
#include "mempool.h"
#include "kernel_config.h"
#include "mpu.h"
#include "usercopy.h"
#include "hal.h"
#include <stdint.h>
#include <string.h>

#if CAP_ENABLE
#include "capability.h"
#endif
#if CAP_RESTART_SUBSET
#include "cap_subset.h"
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

#define SYSCALL_PATH_MAX   128
#define SYSCALL_IO_CHUNK    64

static int syscall_current_is_user(void) {
    tcb_t *cur = sched_get_current();
    return cur != NULL && (cur->attrs & TASK_ATTR_USER) != 0;
}

/*
 * Blocking IPC primitives still resume in their C caller after wakeup.  That is
 * safe from thread mode, but not from SVC handler mode because PendSV cannot run
 * while the handler is spinning.  Until P3-7 syscall continuations land, user
 * IPC syscalls must use nonblocking timeout==0 semantics.
 */
static kern_err_t syscall_block_sleep(uint32_t ticks) {
    tcb_t *cur = sched_get_current();
    if (cur == NULL) {
        return KERN_ERR_STATE;
    }
    if (hal_irq_get_active() >= 0) {
        return KERN_ERR_ISR;
    }
    if (ticks == 0) {
        return KERN_OK;
    }

    uint32_t crit = hal_enter_critical();
    sched_remove_ready(cur);
    cur->syscall_blocked = 1;
    cur->state = TASK_STATE_BLOCKED;
    cur->block_reason = BLOCK_REASON_SLEEP;
    cur->block_obj = NULL;
    cur->block_result = KERN_OK;
    cur->wake_tick = sched_get_tick_count() + ticks;
    hal_exit_critical(crit);

    return KERN_SYSCALL_BLOCKED;
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
    return syscall_block_sleep(a1);
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

    if (!user_access_ok((const void *)(uintptr_t)a2, 1, USER_ACCESS_READ)) {
        return KERN_ERR_PARAM;
    }

    kern_err_t copy_err = strncpy_from_user(name_buf,
                                            (const char *)(uintptr_t)a1,
                                            sizeof(name_buf));
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
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_TASK, CAP_MANAGE);
    if (!obj) return KERN_ERR_CAP;
    return task_suspend((task_id_t)((uintptr_t)obj - 1));
#else
    return task_suspend((task_id_t)a1);
#endif
}

static int sys_task_resume(uint32_t a1, uint32_t a2, uint32_t a3,
                                   uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_TASK, CAP_MANAGE);
    if (!obj) return KERN_ERR_CAP;
    return task_resume((task_id_t)((uintptr_t)obj - 1));
#else
    return task_resume((task_id_t)a1);
#endif
}

static int sys_task_delete(uint32_t a1, uint32_t a2, uint32_t a3,
                                   uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_TASK, CAP_MANAGE);
    if (!obj) return KERN_ERR_CAP;
    task_id_t id = (task_id_t)((uintptr_t)obj - 1);
    kern_err_t ret = task_delete(id);
    if (ret == KERN_OK) {
        cap_delete((cap_id_t)a1);
    }
    return ret;
#else
    return task_delete((task_id_t)a1);
#endif
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
    U(a1);U(a2);U(a3);U(a4);U(a5);U(a6);
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
    U(a1);U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_SEMAPHORE, CAP_READ);
    if (!obj) return KERN_ERR_CAP;
    return sem_wait_syscall((sem_id_t)((uintptr_t)obj - 1), a2);
#else
    return sem_wait_syscall((sem_id_t)a1, a2);
#endif
}

static int sys_sem_post(uint32_t a1, uint32_t a2, uint32_t a3,
                                uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a1);U(a2);U(a3);U(a4);U(a5);U(a6);
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
    return mutex_lock_syscall((mutex_id_t)((uintptr_t)obj - 1), a2);
#else
    return mutex_lock_syscall((mutex_id_t)a1, a2);
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
    uint8_t msg[KERN_MSG_MAX_SIZE];

    if (!user_access_ok((const void *)(uintptr_t)a2, KERN_MSG_MAX_SIZE, USER_ACCESS_READ)) {
        return KERN_ERR_PARAM;
    }
    kern_err_t copy_err = copy_from_user(msg, (const void *)(uintptr_t)a2,
                                         sizeof(msg));
    if (copy_err != KERN_OK) {
        return copy_err;
    }
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_MQUEUE, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;
    return mqueue_send_syscall((queue_id_t)((uintptr_t)obj - 1), msg, a3);
#else
    return mqueue_send_syscall((queue_id_t)a1, msg, a3);
#endif
}

static int sys_mqueue_recv(uint32_t a1, uint32_t a2, uint32_t a3,
                                   uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
    uint8_t msg[KERN_MSG_MAX_SIZE];

    if (!user_access_ok((void *)(uintptr_t)a2, KERN_MSG_MAX_SIZE, USER_ACCESS_WRITE)) {
        return KERN_ERR_PARAM;
    }
    if (a3 != 0) {
#if CAP_ENABLE
        void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_MQUEUE, CAP_READ);
        if (!obj) return KERN_ERR_CAP;
        return mqueue_recv_syscall((queue_id_t)((uintptr_t)obj - 1),
                                   (void *)(uintptr_t)a2, a3);
#else
        return mqueue_recv_syscall((queue_id_t)a1, (void *)(uintptr_t)a2, a3);
#endif
    }

    memset(msg, 0, sizeof(msg));
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_MQUEUE, CAP_READ);
    if (!obj) return KERN_ERR_CAP;
    kern_err_t ret = mqueue_recv((queue_id_t)((uintptr_t)obj - 1), msg, a3);
#else
    kern_err_t ret = mqueue_recv((queue_id_t)a1, msg, a3);
#endif
    if (ret != KERN_OK) {
        return ret;
    }
    return copy_to_user((void *)(uintptr_t)a2, msg, sizeof(msg));
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
    return event_wait_syscall((event_id_t)((uintptr_t)obj - 1), a2, 0, a3);
#else
    return event_wait_syscall((event_id_t)a1, a2, 0, a3);
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
    kern_err_t copy_err = strncpy_from_user(name_buf,
                                            (const char *)(uintptr_t)a1,
                                            sizeof(name_buf));
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
    uint8_t msg[KERN_EP_MSG_SIZE];
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_ENDPOINT, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;
    ep_id_t ep_id = (ep_id_t)((uintptr_t)obj - 1);
#else
    ep_id_t ep_id = (ep_id_t)a1;
#endif
    uint16_t msg_size = endpoint_msg_size(ep_id);
    if (msg_size == 0U) return KERN_ERR_PARAM;
    if (!user_access_ok((const void *)(uintptr_t)a2, msg_size,
                        USER_ACCESS_READ)) {
        return KERN_ERR_PARAM;
    }
    memset(msg, 0, sizeof(msg));
    kern_err_t copy_err = copy_from_user(msg, (const void *)(uintptr_t)a2,
                                         msg_size);
    if (copy_err != KERN_OK) return copy_err;
    return endpoint_send_syscall(ep_id,
                                 msg,
                                 (void *)(uintptr_t)a2,
                                 a3);
}

static int sys_ep_send_caps(uint32_t a1, uint32_t a2, uint32_t a3,
                                    uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a6);
#if CAP_ENABLE
    uint8_t msg[KERN_EP_MSG_SIZE];
    ipc_cap_xfer_t xfers[IPC_CAPS_MAX];
    uint8_t cap_count = (uint8_t)a4;

    if (a4 > IPC_CAPS_MAX) {
        return KERN_ERR_PARAM;
    }
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_ENDPOINT, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;
    ep_id_t ep_id = (ep_id_t)((uintptr_t)obj - 1);
    uint16_t msg_size = endpoint_msg_size(ep_id);
    if (msg_size == 0U) return KERN_ERR_PARAM;
    if (!user_access_ok((const void *)(uintptr_t)a2, msg_size,
                        USER_ACCESS_READ)) {
        return KERN_ERR_PARAM;
    }
    memset(msg, 0, sizeof(msg));
    kern_err_t copy_err = copy_from_user(msg, (const void *)(uintptr_t)a2,
                                         msg_size);
    if (copy_err != KERN_OK) {
        return copy_err;
    }
    if (cap_count > 0) {
        if (!user_access_ok((const void *)(uintptr_t)a3,
                            (uint32_t)cap_count * sizeof(xfers[0]),
                            USER_ACCESS_READ)) {
            return KERN_ERR_PARAM;
        }
        copy_err = copy_from_user(xfers, (const void *)(uintptr_t)a3,
                                  (uint32_t)cap_count * sizeof(xfers[0]));
        if (copy_err != KERN_OK) {
            return copy_err;
        }
    }

    return endpoint_send_caps_syscall(ep_id,
                                      msg,
                                      (void *)(uintptr_t)a2,
                                      xfers,
                                      cap_count,
                                      a5);
#else
    U(a1);U(a2);U(a3);U(a4);U(a5);
    return KERN_ERR_CAP;
#endif
}

static int sys_ep_recv(uint32_t a1, uint32_t a2, uint32_t a3,
                               uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
    uint8_t msg[KERN_EP_MSG_SIZE];

#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_ENDPOINT, CAP_READ);
    if (!obj) return KERN_ERR_CAP;
    ep_id_t ep_id = (ep_id_t)((uintptr_t)obj - 1);
#else
    ep_id_t ep_id = (ep_id_t)a1;
#endif
    uint16_t msg_size = endpoint_msg_size(ep_id);
    if (msg_size == 0U) return KERN_ERR_PARAM;
    if (!user_access_ok((void *)(uintptr_t)a2, msg_size,
                        USER_ACCESS_WRITE)) return KERN_ERR_PARAM;

    if (a3 != 0) {
        return endpoint_recv_syscall(ep_id, (void *)(uintptr_t)a2, a3);
    }

    memset(msg, 0, sizeof(msg));
    kern_err_t ret = endpoint_recv(ep_id, msg, a3);
    if (ret != KERN_OK) {
        return ret;
    }
    return copy_to_user((void *)(uintptr_t)a2, msg, msg_size);
}

static int sys_ep_recv_caps(uint32_t a1, uint32_t a2, uint32_t a3,
                                    uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_ENDPOINT, CAP_READ);
    if (!obj) return KERN_ERR_CAP;
    ep_id_t ep_id = (ep_id_t)((uintptr_t)obj - 1);
    uint16_t msg_size = endpoint_msg_size(ep_id);
    if (msg_size == 0U) return KERN_ERR_PARAM;
    if (!user_access_ok((void *)(uintptr_t)a2, msg_size,
                        USER_ACCESS_WRITE)) {
        return KERN_ERR_PARAM;
    }
    if (!user_access_ok((void *)(uintptr_t)a3,
                        IPC_CAPS_MAX * sizeof(cap_id_t),
                        USER_ACCESS_WRITE)) {
        return KERN_ERR_PARAM;
    }
    if (!user_access_ok((void *)(uintptr_t)a4, sizeof(uint8_t),
                        USER_ACCESS_WRITE)) {
        return KERN_ERR_PARAM;
    }

    return endpoint_recv_caps_syscall(ep_id,
                                      (void *)(uintptr_t)a2,
                                      (cap_id_t *)(uintptr_t)a3,
                                      (uint8_t *)(uintptr_t)a4,
                                      a5);
#else
    U(a1);U(a2);U(a3);U(a4);U(a5);
    return KERN_ERR_CAP;
#endif
}

static int sys_ep_reply(uint32_t a1, uint32_t a2, uint32_t a3,
                                uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a3);U(a4);U(a5);U(a6);
    uint8_t msg[KERN_EP_MSG_SIZE];

    if (!user_access_ok((const void *)(uintptr_t)a2, KERN_EP_MSG_SIZE, USER_ACCESS_READ)) {
        return KERN_ERR_PARAM;
    }
    kern_err_t copy_err = copy_from_user(msg, (const void *)(uintptr_t)a2,
                                         sizeof(msg));
    if (copy_err != KERN_OK) {
        return copy_err;
    }
#if CAP_ENABLE
    void *reply = cap_resolve((cap_id_t)a1, CAP_OBJ_REPLY, CAP_WRITE);
    if (reply) {
        return endpoint_reply_cap(reply, msg);
    }
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_ENDPOINT, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;
    return endpoint_reply((ep_id_t)((uintptr_t)obj - 1), msg);
#else
    return endpoint_reply((ep_id_t)a1, msg);
#endif
}

static int sys_ep_take_reply(uint32_t a1, uint32_t a2, uint32_t a3,
                                     uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_ENDPOINT, CAP_READ);
    if (!obj) return KERN_ERR_CAP;
    return (int)endpoint_take_reply_cap((ep_id_t)((uintptr_t)obj - 1));
#else
    U(a1);
    return KERN_ERR_CAP;
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
    uint8_t msg[KERN_CH_MSG_SIZE];

    if (!user_access_ok((const void *)(uintptr_t)a2, KERN_CH_MSG_SIZE, USER_ACCESS_READ)) {
        return KERN_ERR_PARAM;
    }
    kern_err_t copy_err = copy_from_user(msg, (const void *)(uintptr_t)a2,
                                         sizeof(msg));
    if (copy_err != KERN_OK) {
        return copy_err;
    }
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_CHANNEL, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;
    return channel_send_syscall((ch_id_t)((uintptr_t)obj - 1), msg, a3);
#else
    return channel_send_syscall((ch_id_t)a1, msg, a3);
#endif
}

static int sys_ch_send_caps(uint32_t a1, uint32_t a2, uint32_t a3,
                                    uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a6);
#if CAP_ENABLE
    uint8_t msg[KERN_CH_MSG_SIZE];
    ipc_cap_xfer_t xfers[IPC_CAPS_MAX];
    uint8_t cap_count = (uint8_t)a4;

    if (a4 > IPC_CAPS_MAX) {
        return KERN_ERR_PARAM;
    }
    if (!user_access_ok((const void *)(uintptr_t)a2, KERN_CH_MSG_SIZE,
                        USER_ACCESS_READ)) {
        return KERN_ERR_PARAM;
    }
    kern_err_t copy_err = copy_from_user(msg, (const void *)(uintptr_t)a2,
                                         sizeof(msg));
    if (copy_err != KERN_OK) {
        return copy_err;
    }
    if (cap_count > 0) {
        if (!user_access_ok((const void *)(uintptr_t)a3,
                            (uint32_t)cap_count * sizeof(xfers[0]),
                            USER_ACCESS_READ)) {
            return KERN_ERR_PARAM;
        }
        copy_err = copy_from_user(xfers, (const void *)(uintptr_t)a3,
                                  (uint32_t)cap_count * sizeof(xfers[0]));
        if (copy_err != KERN_OK) {
            return copy_err;
        }
    }

    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_CHANNEL, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;
    return channel_send_caps_syscall((ch_id_t)((uintptr_t)obj - 1),
                                     msg, xfers, cap_count, a5);
#else
    U(a1);U(a2);U(a3);U(a4);U(a5);
    return KERN_ERR_CAP;
#endif
}

static int sys_ch_recv(uint32_t a1, uint32_t a2, uint32_t a3,
                               uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
    uint8_t msg[KERN_CH_MSG_SIZE];

    if (!user_access_ok((void *)(uintptr_t)a2, KERN_CH_MSG_SIZE, USER_ACCESS_WRITE)) {
        return KERN_ERR_PARAM;
    }
    if (a3 != 0) {
#if CAP_ENABLE
        void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_CHANNEL, CAP_READ);
        if (!obj) return KERN_ERR_CAP;
        return channel_recv_syscall((ch_id_t)((uintptr_t)obj - 1),
                                    (void *)(uintptr_t)a2, a3);
#else
        return channel_recv_syscall((ch_id_t)a1, (void *)(uintptr_t)a2, a3);
#endif
    }

    memset(msg, 0, sizeof(msg));
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_CHANNEL, CAP_READ);
    if (!obj) return KERN_ERR_CAP;
    kern_err_t ret = channel_recv((ch_id_t)((uintptr_t)obj - 1), msg, a3);
#else
    kern_err_t ret = channel_recv((ch_id_t)a1, msg, a3);
#endif
    if (ret != KERN_OK) {
        return ret;
    }
    return copy_to_user((void *)(uintptr_t)a2, msg, sizeof(msg));
}

static int sys_ch_recv_caps(uint32_t a1, uint32_t a2, uint32_t a3,
                                    uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a6);
#if CAP_ENABLE
    if (!user_access_ok((void *)(uintptr_t)a2, KERN_CH_MSG_SIZE,
                        USER_ACCESS_WRITE)) {
        return KERN_ERR_PARAM;
    }
    if (!user_access_ok((void *)(uintptr_t)a3,
                        IPC_CAPS_MAX * sizeof(cap_id_t),
                        USER_ACCESS_WRITE)) {
        return KERN_ERR_PARAM;
    }
    if (!user_access_ok((void *)(uintptr_t)a4, sizeof(uint8_t),
                        USER_ACCESS_WRITE)) {
        return KERN_ERR_PARAM;
    }

    void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_CHANNEL, CAP_READ);
    if (!obj) return KERN_ERR_CAP;
    return channel_recv_caps_syscall((ch_id_t)((uintptr_t)obj - 1),
                                     (void *)(uintptr_t)a2,
                                     (cap_id_t *)(uintptr_t)a3,
                                     (uint8_t *)(uintptr_t)a4,
                                     a5);
#else
    U(a1);U(a2);U(a3);U(a4);U(a5);
    return KERN_ERR_CAP;
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

    if (syscall_current_is_user() && a2 != 0) {
        return KERN_ERR_PERM;
    }

    kern_err_t copy_err = strncpy_from_user(name_buf,
                                            (const char *)(uintptr_t)a1,
                                            sizeof(name_buf));
    if (copy_err != KERN_OK) {
        return copy_err;
    }

    if (a2 != 0 &&
        !user_access_ok((const void *)(uintptr_t)a2, 1, USER_ACCESS_READ)) {
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

static int sys_timer_bind(uint32_t a1, uint32_t a2, uint32_t a3,
                          uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *timer_obj = cap_resolve((cap_id_t)a1, CAP_OBJ_TIMER, CAP_WRITE);
    if (!timer_obj) return KERN_ERR_CAP;
    void *ep_obj = cap_resolve((cap_id_t)a2, CAP_OBJ_ENDPOINT, CAP_WRITE);
    if (!ep_obj) return KERN_ERR_CAP;
    return timer_bind_endpoint((timer_id_t)((uintptr_t)timer_obj - 1),
                               (ep_id_t)((uintptr_t)ep_obj - 1),
                               a3);
#else
    return timer_bind_endpoint((timer_id_t)a1, (ep_id_t)a2, a3);
#endif
}

/*============================================================================
 * 内存管理
 *============================================================================*/

static int sys_mem_alloc(uint32_t a1, uint32_t a2, uint32_t a3,
                                 uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
    if (a1 == 0) return KERN_ERR_PARAM;
#if CAP_ENABLE
    cap_id_t cap = kmem_alloc_cap((size_t)a1,
                                  CAP_READ | CAP_WRITE | CAP_MANAGE |
                                  CAP_TRANSFER);
    if (cap < 0) return KERN_ERR_RESOURCE;
    return (int)cap;
#else
    void *ptr = kmalloc(a1);
    if (!ptr) return KERN_ERR_RESOURCE;
    return (kern_err_t)(uintptr_t)ptr;
#endif
}

static int sys_mem_free(uint32_t a1, uint32_t a2, uint32_t a3,
                                uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    return kmem_free_cap((cap_id_t)a1);
#else
    kfree((void *)a1);
    return KERN_OK;
#endif
}

static int sys_mem_size(uint32_t a1, uint32_t a2, uint32_t a3,
                                uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    void *base = NULL;
    size_t size = 0;
    kern_err_t err = kmem_get_bounds((cap_id_t)a1, &base, &size);
    U(base);
    if (err != KERN_OK) {
        return err;
    }
    if (size > (size_t)INT32_MAX) {
        return KERN_ERR_OVERFLOW;
    }
    return (int)size;
#else
    return KERN_ERR_CAP;
#endif
}

static int sys_shm_create(uint32_t a1, uint32_t a2, uint32_t a3,
                          uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a1);U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE && MPU_ENABLE && MEM_DYNAMIC
    if (a1 == 0) {
        return KERN_ERR_PARAM;
    }
    if (syscall_current_is_user()) {
        return KERN_ERR_PERM;
    }

    uint8_t rights = (uint8_t)a2;
    if (rights == 0) {
        rights = CAP_READ | CAP_WRITE | CAP_MANAGE |
                 CAP_TRANSFER | CAP_GRANT;
    }
    if ((rights & ~(CAP_READ | CAP_WRITE | CAP_MANAGE |
                    CAP_TRANSFER | CAP_GRANT)) != 0) {
        return KERN_ERR_PARAM;
    }

    cap_id_t cap = kshm_create_aligned_cap((size_t)a1, rights);
    if (cap < 0) {
        return KERN_ERR_RESOURCE;
    }
    return (int)cap;
#else
    return KERN_ERR_CAP;
#endif
}

static int sys_shm_map(uint32_t a1, uint32_t a2, uint32_t a3,
                       uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a1);U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE && MPU_ENABLE
    tcb_t *current = sched_get_current();
    if (current == NULL || (current->attrs & TASK_ATTR_USER) == 0) {
        return KERN_ERR_PERM;
    }

    void *addr = NULL;
    kern_err_t err = kshm_map_to_task(current, (cap_id_t)a1,
                                      (uint8_t)a2, &addr);
    if (err != KERN_OK) {
        return err;
    }

    mpu_load_task_regions(current);
    return (int)(uintptr_t)addr;
#else
    return KERN_ERR_CAP;
#endif
}

static int sys_shm_unmap(uint32_t a1, uint32_t a2, uint32_t a3,
                         uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a1);U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE && MPU_ENABLE
    tcb_t *current = sched_get_current();
    if (current == NULL || (current->attrs & TASK_ATTR_USER) == 0) {
        return KERN_ERR_PERM;
    }

    kern_err_t err = kshm_unmap_from_task(current, (cap_id_t)a1);
    if (err == KERN_OK || err == KERN_ERR_CAP) {
        mpu_load_task_regions(current);
    }
    return err;
#else
    return KERN_ERR_CAP;
#endif
}

/*============================================================================
 * 中断管理
 *============================================================================*/

static int sys_irq_register(uint32_t a1, uint32_t a2, uint32_t a3,
                                    uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
    if (syscall_current_is_user()) {
        return KERN_ERR_PERM;
    }
    return irq_register((int16_t)a1, (isr_func_t)a2, (uint8_t)a3);
}

static int sys_irq_bind(uint32_t a1, uint32_t a2, uint32_t a3,
                                uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
#if CAP_ENABLE && IPC_ENDPOINT
    void *ep_obj = cap_resolve((cap_id_t)a2, CAP_OBJ_ENDPOINT, CAP_WRITE);
    if (ep_obj == NULL) {
        return KERN_ERR_CAP;
    }
    ep_id_t ep_id = (ep_id_t)((uintptr_t)ep_obj - 1);
    return kirq_bind_endpoint((cap_id_t)a1, ep_id, a3);
#else
    U(a1);U(a2);U(a3);
    return KERN_ERR_CAP;
#endif
}

static int sys_bh_create(uint32_t a1, uint32_t a2, uint32_t a3,
                                 uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a3);U(a4);U(a5);U(a6);
    if (syscall_current_is_user()) {
        return KERN_ERR_PERM;
    }
    return (kern_err_t)bh_create((bh_handler_t)a1, (void *)a2);
}

static int sys_bh_schedule(uint32_t a1, uint32_t a2, uint32_t a3,
                                   uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
#if CAP_ENABLE
    if (syscall_current_is_user()) {
        void *obj = cap_resolve((cap_id_t)a1, CAP_OBJ_BH, CAP_WRITE);
        if (!obj) return KERN_ERR_CAP;
        return bh_schedule((int16_t)((uintptr_t)obj - 1));
    }
#endif
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

static int sys_cap_type(uint32_t a1, uint32_t a2, uint32_t a3,
                                uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
    uint8_t obj_type = 0;
    kern_err_t err = cap_get_type((cap_id_t)a1, &obj_type);
    return (err == KERN_OK) ? (int)obj_type : (int)err;
}

static int sys_cap_rights(uint32_t a1, uint32_t a2, uint32_t a3,
                                  uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
    uint8_t rights = 0;
    kern_err_t err = cap_get_rights((cap_id_t)a1, &rights);
    return (err == KERN_OK) ? (int)rights : (int)err;
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
    kern_err_t copy_err = strncpy_from_user(path,
                                            (const char *)(uintptr_t)a1,
                                            sizeof(path));
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
    uint8_t bounce[SYSCALL_IO_CHUNK];
    uint8_t *user_buf = (uint8_t *)(uintptr_t)a2;
    uint32_t total = 0;

    if (!user_access_ok(user_buf, a3, USER_ACCESS_WRITE)) {
        return KERN_ERR_PARAM;
    }

    while (total < a3) {
        uint32_t chunk = a3 - total;
        if (chunk > sizeof(bounce)) {
            chunk = sizeof(bounce);
        }

        int n = vfs_read((int)a1, bounce, chunk);
        if (n <= 0) {
            return total ? (int)total : n;
        }

        kern_err_t err = copy_to_user(user_buf + total, bounce, (uint32_t)n);
        if (err != KERN_OK) {
            return total ? (int)total : err;
        }

        total += (uint32_t)n;
        if ((uint32_t)n < chunk) {
            break;
        }
    }

    return (int)total;
}

static int sys_write(uint32_t a1, uint32_t a2, uint32_t a3,
                             uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
    uint8_t bounce[SYSCALL_IO_CHUNK];
    const uint8_t *user_buf = (const uint8_t *)(uintptr_t)a2;
    uint32_t total = 0;

    if (!user_access_ok(user_buf, a3, USER_ACCESS_READ)) {
        return KERN_ERR_PARAM;
    }

    while (total < a3) {
        uint32_t chunk = a3 - total;
        if (chunk > sizeof(bounce)) {
            chunk = sizeof(bounce);
        }

        kern_err_t err = copy_from_user(bounce, user_buf + total, chunk);
        if (err != KERN_OK) {
            return total ? (int)total : err;
        }

        int n = vfs_write((int)a1, bounce, chunk);
        if (n <= 0) {
            return total ? (int)total : n;
        }

        total += (uint32_t)n;
        if ((uint32_t)n < chunk) {
            break;
        }
    }

    return (int)total;
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

static int sys_readdir(uint32_t a1, uint32_t a2, uint32_t a3,
                               uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a3);U(a4);U(a5);U(a6);
    dirent_t entry;
    void *user_entry = (void *)(uintptr_t)a2;

    if (!user_access_ok(user_entry, sizeof(entry), USER_ACCESS_WRITE)) {
        return KERN_ERR_PARAM;
    }

    kern_err_t err = vfs_readdir((int)a1, &entry);
    if (err != KERN_OK) {
        return err;
    }
    return copy_to_user(user_entry, &entry, sizeof(entry));
}

static int sys_unlink(uint32_t a1, uint32_t a2, uint32_t a3,
                              uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
    char path[SYSCALL_PATH_MAX];
    kern_err_t copy_err = strncpy_from_user(path,
                                            (const char *)(uintptr_t)a1,
                                            sizeof(path));
    if (copy_err != KERN_OK) {
        return copy_err;
    }
    return vfs_unlink(path);
}

static int sys_mkdir(uint32_t a1, uint32_t a2, uint32_t a3,
                             uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
    char path[SYSCALL_PATH_MAX];
    kern_err_t copy_err = strncpy_from_user(path,
                                            (const char *)(uintptr_t)a1,
                                            sizeof(path));
    if (copy_err != KERN_OK) {
        return copy_err;
    }
    return vfs_mkdir(path);
}

static int sys_stat(uint32_t a1, uint32_t a2, uint32_t a3,
                            uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a3);U(a4);U(a5);U(a6);
    char path[SYSCALL_PATH_MAX];
    vfs_stat_t st;
    void *user_st = (void *)(uintptr_t)a2;

    if (!user_access_ok(user_st, sizeof(st), USER_ACCESS_WRITE)) {
        return KERN_ERR_PARAM;
    }

    kern_err_t copy_err = strncpy_from_user(path,
                                            (const char *)(uintptr_t)a1,
                                            sizeof(path));
    if (copy_err != KERN_OK) {
        return copy_err;
    }

    kern_err_t err = vfs_stat(path, &st);
    if (err != KERN_OK) {
        return err;
    }
    return copy_to_user(user_st, &st, sizeof(st));
}

#endif /* VFS_ENABLE */

/*============================================================================
 * Phase 2 — fault endpoint subscription
 *============================================================================*/

#if FAULT_ENDPOINT
extern ep_id_t kern_fault_ep;  /* defined in fault_endpoint.c */

/**
 * sys_fault_subscribe — let a user-mode supervisor learn the fault ep id.
 *
 * No args. Returns the ep_id (>= 0) on success, or:
 *   KERN_ERR_NOSYS  — FAULT_ENDPOINT disabled at compile time
 *   KERN_ERR_STATE  — fault endpoint not yet initialized
 *
 * Security note: any user task can call this today. Phase 2 S4 will gate
 * behind a capability so only the designated supervisor can subscribe.
 */
static int sys_fault_subscribe(uint32_t a1, uint32_t a2, uint32_t a3,
                               uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a1);U(a2);U(a3);U(a4);U(a5);U(a6);
    if (kern_fault_ep == KERN_INVALID_ID) {
        return KERN_ERR_STATE;
    }
#if CAP_ENABLE
    /* Hand the caller a CAP_FULL capability over the fault endpoint. The
     * supervisor needs CAP_READ to recv fault events AND CAP_WRITE to bind a
     * one-shot timer onto this same endpoint (the timer delivers its
     * backoff-expiry notification here, so fault events and timer events
     * arrive on the single ep the supervisor already recv's on — there is no
     * multi-endpoint wait primitive). */
    tcb_t *cur = sched_get_current();
    cap_id_t cap = cap_create_for(cur,
                                  (void *)(uintptr_t)(kern_fault_ep + 1),
                                  CAP_OBJ_ENDPOINT, CAP_FULL);
    if (cap < 0) {
        return KERN_ERR_RESOURCE;
    }
    return (int)cap;
#else
    return (int)kern_fault_ep;
#endif
}
#else
static int sys_fault_subscribe(uint32_t a1, uint32_t a2, uint32_t a3,
                               uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a1);U(a2);U(a3);U(a4);U(a5);U(a6);
    return KERN_ERR_NOSYS;
}
#endif /* FAULT_ENDPOINT */

/*============================================================================
 * Phase 2 §2.4 — task restart with capability subset
 *============================================================================*/

#if CAP_RESTART_SUBSET && FAULT_ENDPOINT

/**
 * sys_task_restart — atomically recreate a faulted task with a reduced cap.
 *
 * args: name(ptr), entry(ptr), arg(ptr), priority, stack_size, cap_rights_mask
 *
 * Flow:
 *   1. create the new user task (task_create_user)
 *   2. find a parent TASK cap in the CALLER's cspace that carries CAP_GRANT
 *      (the supervisor's self-cap, granted at spawn time)
 *   3. cap_derive_for_restart() to install a CAP_GRANT-stripped child cap
 *      into the new task's cspace
 *   4. start the new task; return its TASK cap to the supervisor
 *
 * If no parent cap with CAP_GRANT is held, the task is still created/started
 * but receives NO caps (degraded, but not a panic). The caller can detect
 * missing authority via the returned rights.
 *
 * Returns the new task's cap id (>=0) on success, or a negative kern_err_t.
 */
static int sys_task_restart(uint32_t a1, uint32_t a2, uint32_t a3,
                            uint32_t a4, uint32_t a5, uint32_t a6) {
    char name_buf[KERN_TASK_NAME_LEN];
    task_func_t entry = (task_func_t)(uintptr_t)a2;
    void *arg         = (void *)(uintptr_t)a3;
    uint8_t priority  = (uint8_t)a4;
    uint32_t stack_sz = a5;
    uint8_t rights_mask = (uint8_t)a6;

    if (!user_access_ok((const void *)(uintptr_t)a2, 1, USER_ACCESS_READ)) {
        return KERN_ERR_PARAM;
    }
    kern_err_t copy_err = strncpy_from_user(name_buf,
                                            (const char *)(uintptr_t)a1,
                                            sizeof(name_buf));
    if (copy_err != KERN_OK) {
        return copy_err;
    }

    tcb_t *caller = sched_get_current();
    if (caller == NULL || (caller->attrs & TASK_ATTR_USER) == 0) {
        return KERN_ERR_PERM;
    }

#if MPU_ENABLE
    task_id_t id = task_create_user(name_buf[0] ? name_buf : NULL,
                                    entry, arg, priority, stack_sz);
#else
    task_id_t id = task_create(name_buf[0] ? name_buf : NULL,
                               entry, arg, priority, stack_sz);
#endif
    if (id < 0) {
        return id;
    }

    tcb_t *new_task = task_get_tcb(id);

    /* Install a reduced-rights cap into the new task. We need a parent TASK
     * cap held by the caller with CAP_GRANT. Walk the caller's cspace. */
    int granted = 0;
    if (caller != NULL && (caller->attrs & TASK_ATTR_USER) != 0) {
        for (int i = 0; i < KERN_TASK_CAP_SLOTS; i++) {
            uint32_t bit = (uint32_t)BIT(i);
            if ((caller->capabilities & bit) == 0) {
                continue;
            }
            cap_id_t cand = caller->cap_set[i];
            uint8_t obj_type = 0;
            uint8_t rights = 0;
            if (cap_get_type_for(caller, cand, &obj_type) != KERN_OK ||
                obj_type != CAP_OBJ_TASK) {
                continue;
            }
            if (cap_get_rights_for(caller, cand, &rights) != KERN_OK ||
                (rights & CAP_GRANT) == 0) {
                continue;
            }
            /* Found a derive-capable parent. Install reduced cap into new task. */
            cap_id_t child = cap_derive_for_restart(caller, cand,
                                                    new_task, rights_mask);
            if (child >= 0) {
                granted = 1;
            }
            break;  /* first suitable parent wins */
        }
    }
    (void)granted;  /* degraded mode (no cap) is non-fatal */

    /* Mint a management TASK cap for the CALLER so it can start/manage the
     * restarted task — mirrors sys_task_create's contract. */
    cap_id_t mgr_cap = cap_create_for(caller,
                                      (void *)(uintptr_t)(id + 1),
                                      CAP_OBJ_TASK, CAP_FULL);
    if (mgr_cap < 0) {
        task_delete(id);
        return KERN_ERR_RESOURCE;
    }

    kern_err_t start_err = task_start(id);
    if (start_err != KERN_OK) {
        cap_delete(mgr_cap);
        task_delete(id);
        return start_err;
    }

    return (int)mgr_cap;
}
#else
static int sys_task_restart(uint32_t a1, uint32_t a2, uint32_t a3,
                            uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a1);U(a2);U(a3);U(a4);U(a5);U(a6);
    return KERN_ERR_NOSYS;
}
#endif /* CAP_RESTART_SUBSET && FAULT_ENDPOINT */

/*============================================================================
 * Time: current scheduler tick (USER-safe read of the kernel tick counter).
 *============================================================================*/

static int sys_get_tick(uint32_t a1, uint32_t a2, uint32_t a3,
                        uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a1);U(a2);U(a3);U(a4);U(a5);U(a6);
    extern uint32_t sched_get_tick_count(void);
    return (int)sched_get_tick_count();
}

/*============================================================================
 * Phase 3 §3.3 — block device flash operations (user → kernel delegation)
 *============================================================================*/

#if BLOCK_DEVICE
#include "flash_block.h"

/* op codes for sys_flash_op */
#define FLASH_OP_READ    0
#define FLASH_OP_ERASE   1
#define FLASH_OP_PROGRAM 2

/**
 * sys_flash_op — read/erase/program the FS region of onboard flash.
 *
 * args: op(a1), offset-within-FS(a2), buf(a3), count(a4)
 *
 * The erase/program primitives require IRQs disabled, which a user task
 * cannot do, so the work is done here in handler context. buf is validated
 * for user access; offset+count are bounds-checked against the FS region by
 * flash_block_*.
 */
static int sys_flash_op(uint32_t a1, uint32_t a2, uint32_t a3,
                        uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a5);U(a6);
    uint32_t op    = a1;
    uint32_t offs  = a2;
    void *buf      = (void *)(uintptr_t)a3;
    uint32_t count = a4;

    switch (op) {
    case FLASH_OP_READ:
        if (!user_access_ok(buf, count, USER_ACCESS_WRITE)) {
            return KERN_ERR_PARAM;
        }
        return (int)flash_block_read(offs, buf, count);
    case FLASH_OP_ERASE:
        return (int)flash_block_erase(offs, count);
    case FLASH_OP_PROGRAM:
        if (!user_access_ok(buf, count, USER_ACCESS_READ)) {
            return KERN_ERR_PARAM;
        }
        return (int)flash_block_program(offs, buf, count);
    default:
        return KERN_ERR_PARAM;
    }
}
#else
static int sys_flash_op(uint32_t a1, uint32_t a2, uint32_t a3,
                        uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a1);U(a2);U(a3);U(a4);U(a5);U(a6);
    return KERN_ERR_NOSYS;
}
#endif /* BLOCK_DEVICE */

/*============================================================================
 * MMIO mapping — user-mode driver peripheral access (core completion #2)
 *============================================================================*/

#if MPU_ENABLE && CAP_ENABLE
static int sys_mmio_map(uint32_t a1, uint32_t a2, uint32_t a3,
                        uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a4);U(a5);U(a6);
    cap_id_t cap = (cap_id_t)a1;
    uint8_t  rights = (uint8_t)a2;
    void **out_addr = (void **)(uintptr_t)a3;
    extern kern_err_t kmmio_map_to_task(tcb_t *task, cap_id_t cap,
                                        uint8_t rights, void **out_addr);
    tcb_t *cur = sched_get_current();
    if (cur == NULL) {
        return KERN_ERR_STATE;
    }
    void *mapped = NULL;
    kern_err_t e = kmmio_map_to_task(cur, cap, rights, &mapped);
    if (e != KERN_OK) {
        return (int)e;
    }
    /* Write the mapped base back to the user's out_addr pointer. */
    if (out_addr != NULL) {
        if (!user_access_ok(out_addr, sizeof(mapped), USER_ACCESS_WRITE)) {
            (void)kmmio_unmap_from_task(cur, cap);
            return KERN_ERR_PARAM;
        }
        kern_err_t ce = copy_to_user(out_addr, &mapped, sizeof(mapped));
        if (ce != KERN_OK) {
            (void)kmmio_unmap_from_task(cur, cap);
            return (int)ce;
        }
    }
    return KERN_OK;
}

static int sys_mmio_unmap(uint32_t a1, uint32_t a2, uint32_t a3,
                          uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a2);U(a3);U(a4);U(a5);U(a6);
    extern kern_err_t kmmio_unmap_from_task(tcb_t *task, cap_id_t cap);
    tcb_t *cur = sched_get_current();
    if (cur == NULL) {
        return KERN_ERR_STATE;
    }
    return (int)kmmio_unmap_from_task(cur, (cap_id_t)a1);
}
#else
static int sys_mmio_map(uint32_t a1, uint32_t a2, uint32_t a3,
                        uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a1);U(a2);U(a3);U(a4);U(a5);U(a6);
    return KERN_ERR_NOSYS;
}
static int sys_mmio_unmap(uint32_t a1, uint32_t a2, uint32_t a3,
                          uint32_t a4, uint32_t a5, uint32_t a6) {
    U(a1);U(a2);U(a3);U(a4);U(a5);U(a6);
    return KERN_ERR_NOSYS;
}
#endif /* MPU_ENABLE && CAP_ENABLE */

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
    SYSDEF(SYSCALL_EP_TAKE_REPLY, sys_ep_take_reply, 1),
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
    SYSDEF(SYSCALL_TIMER_BIND,    sys_timer_bind,    3),
    SYSDEF(SYSCALL_IRQ_REGISTER,  sys_irq_register,  3),
    SYSDEF(SYSCALL_IRQ_BIND,      sys_irq_bind,      3),
    SYSDEF(SYSCALL_BH_CREATE,     sys_bh_create,     2),
    SYSDEF(SYSCALL_BH_SCHEDULE,   sys_bh_schedule,   1),
    SYSDEF(SYSCALL_MEM_ALLOC,     sys_mem_alloc,     1),
    SYSDEF(SYSCALL_MEM_FREE,      sys_mem_free,      1),
    SYSDEF(SYSCALL_MEM_SIZE,      sys_mem_size,      1),
    SYSDEF(SYSCALL_SHM_CREATE,    sys_shm_create,    2),
    SYSDEF(SYSCALL_SHM_MAP,       sys_shm_map,       2),
    SYSDEF(SYSCALL_SHM_UNMAP,     sys_shm_unmap,     1),
#if CAP_ENABLE
    SYSDEF(SYSCALL_CAP_DERIVE,    sys_cap_derive,    2),
    SYSDEF(SYSCALL_CAP_TRANSFER,  sys_cap_transfer,  2),
    SYSDEF(SYSCALL_CAP_REVOKE,    sys_cap_revoke,    1),
    SYSDEF(SYSCALL_CAP_TYPE,      sys_cap_type,      1),
    SYSDEF(SYSCALL_CAP_RIGHTS,    sys_cap_rights,    1),
#endif
#if VFS_ENABLE
    SYSDEF(SYSCALL_OPEN,          sys_open,          2),
    SYSDEF(SYSCALL_CLOSE,         sys_close,         1),
    SYSDEF(SYSCALL_READ,          sys_read,          3),
    SYSDEF(SYSCALL_WRITE,         sys_write,         3),
    SYSDEF(SYSCALL_IOCTL,         sys_ioctl,         3),
    SYSDEF(SYSCALL_LSEEK,         sys_lseek,         3),
    SYSDEF(SYSCALL_READDIR,       sys_readdir,       2),
    SYSDEF(SYSCALL_UNLINK,        sys_unlink,        1),
    SYSDEF(SYSCALL_MKDIR,         sys_mkdir,         1),
    SYSDEF(SYSCALL_STAT,          sys_stat,          2),
#endif
    SYSDEF(SYSCALL_FAULT_SUBSCRIBE, sys_fault_subscribe, 0),
    SYSDEF(SYSCALL_TASK_RESTART,    sys_task_restart,    6),
    SYSDEF(SYSCALL_GET_TICK,        sys_get_tick,        0),
    SYSDEF(SYSCALL_FLASH_OP,        sys_flash_op,        4),
    SYSDEF(SYSCALL_MMIO_MAP,        sys_mmio_map,        3),
    SYSDEF(SYSCALL_MMIO_UNMAP,      sys_mmio_unmap,      1),
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
