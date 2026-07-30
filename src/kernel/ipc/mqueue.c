/**
 * @file mqueue.c
 * @brief 消息队列实现
 */

#include "mqueue.h"
#include "wait_queue.h"
#include "scheduler.h"
#include "kernel_config.h"
#include "hal.h"
#include "spinlock.h"
#include "syscall.h"
#include "continuation.h"
#include "capability.h"
#include <string.h>

/*============================================================================
 * 静态分配的消息队列池和缓冲区
 *============================================================================*/

static mqueue_t mqueue_pool[KERN_MAX_MQUEUES];
static uint32_t mqueue_used_bitmap;
static irq_spinlock_t mqueue_lock; /* M1: SMP safe */

// 消息缓冲区池 (每个队列独立缓冲区)
static uint8_t mqueue_buffers[KERN_MAX_MQUEUES][KERN_MQUEUE_DEPTH * KERN_MSG_MAX_SIZE]
    __attribute__((aligned(4)));

#if SYSCALL_ENABLE
static uint8_t mqueue_syscall_send_msg[KERNEL_MAX_TASKS][KERN_MSG_MAX_SIZE]
    __attribute__((aligned(4)));
/* M3-Task3: recv continuation 移到 TCB->cont.msg_buf */
#endif

/*============================================================================
 * 内部函数
 *============================================================================*/

// 分配消息队列 ID
static queue_id_t alloc_mqueue_id(void) {
    for (int i = 0; i < KERN_MAX_MQUEUES; i++) {
        if (!(mqueue_used_bitmap & (1U << i)) &&
            !kobj_generation_is_retired(mqueue_pool[i].hdr.generation)) {
            mqueue_used_bitmap |= (1U << i);
            return (queue_id_t)i;
        }
    }
    return KERN_INVALID_ID;
}

// 释放消息队列 ID
static void free_mqueue_id(queue_id_t id) {
    if (id >= 0 && id < KERN_MAX_MQUEUES) {
        mqueue_used_bitmap &= ~(1U << id);
    }
}

// 获取消息队列指针
static mqueue_t *get_mqueue(queue_id_t id) {
    if (id < 0 || id >= KERN_MAX_MQUEUES) {
        return NULL;
    }
    if (!mqueue_pool[id].in_use) {
        return NULL;
    }
    return &mqueue_pool[id];
}

// 内部发送消息 (无锁)
static void mqueue_do_put(mqueue_t *mq, const void *msg) {
    uint8_t *dst = (uint8_t *)mq->buffer + (mq->head * mq->msg_size);
    memcpy(dst, msg, mq->msg_size);

    mq->head = (mq->head + 1) % mq->capacity;
    mq->count++;
}

// 内部接收消息 (无锁)
static void mqueue_do_get(mqueue_t *mq, void *msg) {
    uint8_t *src = (uint8_t *)mq->buffer + (mq->tail * mq->msg_size);
    memcpy(msg, src, mq->msg_size);

    mq->tail = (mq->tail + 1) % mq->capacity;
    mq->count--;
}

static void mqueue_wake_recv_waiter(mqueue_t *mq) {
    if (mq->recv_queue.count == 0 || mq->count == 0) {
        return;
    }

    tcb_t *tcb = wait_queue_get_highest(&mq->recv_queue);
    if (tcb == NULL) {
        return;
    }

#if SYSCALL_ENABLE
    if (tcb->cont.active &&
        tcb->id >= 0 && tcb->id < KERNEL_MAX_TASKS &&
        tcb->cont.msg_buf != NULL) {
        mqueue_do_get(mq, tcb->cont.msg_buf);
        tcb->cont.msg_buf = NULL;
        wait_queue_remove(&mq->recv_queue, tcb);
        tcb->cont.result = KERN_OK;
        tcb->cont.object = NULL;
        sched_wakeup(tcb, KERN_OK);
        return;
    }
#endif

    wait_queue_remove(&mq->recv_queue, tcb);
    tcb->cont.result = KERN_OK;
    tcb->cont.object = NULL;
    sched_wakeup(tcb, KERN_OK);
}

static void mqueue_wake_send_waiter(mqueue_t *mq) {
    if (mq->send_queue.count == 0 || mq->count >= mq->capacity) {
        return;
    }

    tcb_t *tcb = wait_queue_get_highest(&mq->send_queue);
    if (tcb == NULL) {
        return;
    }

#if SYSCALL_ENABLE
    if (tcb->cont.active &&
        tcb->id >= 0 && tcb->id < KERNEL_MAX_TASKS) {
        mqueue_do_put(mq, mqueue_syscall_send_msg[tcb->id]);
        wait_queue_remove(&mq->send_queue, tcb);
        tcb->cont.result = KERN_OK;
        tcb->cont.object = NULL;
        sched_wakeup(tcb, KERN_OK);
        mqueue_wake_recv_waiter(mq);
        return;
    }
#endif

    wait_queue_remove(&mq->send_queue, tcb);
    tcb->cont.result = KERN_OK;
    tcb->cont.object = NULL;
    sched_wakeup(tcb, KERN_OK);
}

/*============================================================================
 * 公开接口实现
 *============================================================================*/

void mqueue_init(void) {
    irq_spin_init_rank(&mqueue_lock, LOCKDEP_RANK_OBJECT);
    memset(mqueue_pool, 0, sizeof(mqueue_pool));
    mqueue_used_bitmap = 0;
#if SYSCALL_ENABLE
    memset(mqueue_syscall_send_msg, 0, sizeof(mqueue_syscall_send_msg));
    /* M3-Task3: recv side table 已移到 TCB->cont */
#endif
}

queue_id_t mqueue_create(uint32_t msg_size, uint32_t capacity) {
    uint32_t crit = irq_spin_lock(&mqueue_lock);

    queue_id_t id = alloc_mqueue_id();
    if (id == KERN_INVALID_ID) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_INVALID_ID;
    }

    // 限制消息大小和容量
    if (msg_size > KERN_MSG_MAX_SIZE) {
        msg_size = KERN_MSG_MAX_SIZE;
    }
    if (capacity > KERN_MQUEUE_DEPTH) {
        capacity = KERN_MQUEUE_DEPTH;
    }

    mqueue_t *mq = &mqueue_pool[id];

    /* M2-Step3a: 初始化对象 header */
    if (mq->hdr.generation == 0) {
        kobj_header_init(&mq->hdr, CAP_OBJ_MQUEUE);
    } else {
        mq->hdr.obj_type = CAP_OBJ_MQUEUE;
    }
    mq->buffer = mqueue_buffers[id];
    mq->msg_size = (uint16_t)msg_size;
    mq->capacity = (uint16_t)capacity;
    mq->count = 0;
    mq->head = 0;
    mq->tail = 0;
    mq->in_use = 1;

    wait_queue_init(&mq->send_queue);
    wait_queue_init(&mq->recv_queue);

    irq_spin_unlock(&mqueue_lock, crit);
    return id;
}

kern_err_t mqueue_delete(queue_id_t queue_id) {
    uint32_t crit = irq_spin_lock(&mqueue_lock);

    mqueue_t *mq = get_mqueue(queue_id);
    if (mq == NULL) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_PARAM;
    }

    // 唤醒所有发送等待的任务
    tcb_t *tcb = mq->send_queue.head;
    while (tcb) {
        tcb_t *next = tcb->wait_next;
        tcb->wait_next = NULL;
        tcb->wait_prev = NULL;
#if SYSCALL_ENABLE
        if (tcb->id >= 0 && tcb->id < KERNEL_MAX_TASKS) {
            memset(mqueue_syscall_send_msg[tcb->id], 0,
                   sizeof(mqueue_syscall_send_msg[tcb->id]));
        }
#endif
        tcb->cont.result = KERN_ERR_NOEXIST;
        tcb->cont.object = NULL;
        sched_wakeup(tcb, KERN_ERR_NOEXIST);
        tcb = next;
    }

    // 唤醒所有接收等待的任务
    tcb = mq->recv_queue.head;
    while (tcb) {
        tcb_t *next = tcb->wait_next;
        tcb->wait_next = NULL;
        tcb->wait_prev = NULL;
#if SYSCALL_ENABLE
        if (tcb->id >= 0 && tcb->id < KERNEL_MAX_TASKS) {
            tcb->cont.msg_buf = NULL;
        }
#endif
        tcb->cont.result = KERN_ERR_NOEXIST;
        tcb->cont.object = NULL;
        sched_wakeup(tcb, KERN_ERR_NOEXIST);
        tcb = next;
    }

    // 清零并释放
#if CAP_ENABLE
    /* M2-Step1: 撤销所有任务持有的指向此 mqueue 的 cap,避免悬空句柄 */
    (void)cap_revoke_object(mq, CAP_OBJ_MQUEUE);
#endif
    /* M2-Step3a: bump generation 跨 memset 保留 */
    uint32_t next_gen = kobj_header_prepare_reuse(&mq->hdr);
    memset(mq, 0, sizeof(mqueue_t));
    mq->hdr.obj_type   = CAP_OBJ_MQUEUE;
    mq->hdr.generation = next_gen;
    free_mqueue_id(queue_id);

    irq_spin_unlock(&mqueue_lock, crit);
    return KERN_OK;
}

/* M2-Step3a: cap 路径 id ↔ 对象指针 转换。 */
queue_id_t mqueue_id_from_obj(void *obj) {
    if (obj == NULL) return KERN_INVALID_ID;
    mqueue_t *mq = (mqueue_t *)obj;
    queue_id_t id = (queue_id_t)(mq - mqueue_pool);
    if (id < 0 || id >= KERN_MAX_MQUEUES) return KERN_INVALID_ID;
    return id;
}

void *mqueue_obj_for_cap(queue_id_t id) {
    if (id < 0 || id >= KERN_MAX_MQUEUES) return NULL;
    return (void *)&mqueue_pool[id];
}

void mqueue_cleanup_task(void *mqueue_obj, tcb_t *tcb) {
    mqueue_t *mq = (mqueue_t *)mqueue_obj;
    if (mq == NULL || tcb == NULL) return;

    uint32_t crit = irq_spin_lock(&mqueue_lock);
    if (mq >= &mqueue_pool[0] && mq < &mqueue_pool[KERN_MAX_MQUEUES] &&
        mq->in_use) {
        (void)wait_queue_remove_safe(&mq->send_queue, tcb);
        (void)wait_queue_remove_safe(&mq->recv_queue, tcb);
    }
    irq_spin_unlock(&mqueue_lock, crit);
}

kern_err_t mqueue_send(queue_id_t queue_id, const void *msg, uint32_t timeout) {
    uint32_t crit = irq_spin_lock(&mqueue_lock);

    mqueue_t *mq = get_mqueue(queue_id);
    if (mq == NULL) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_PARAM;
    }

    if (mq->count < mq->capacity) {
        mqueue_do_put(mq, msg);

        mqueue_wake_recv_waiter(mq);

        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_OK;
    }

    if (timeout == 0) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_TIMEOUT;
    }

    if (hal_irq_get_active() >= 0) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_ISR;
    }

    tcb_t *current = sched_get_current();
    current->cont.op = BLOCK_REASON_QUEUE;
    current->cont.object = mq;

    wait_queue_add(&mq->send_queue, current);

    /* 从就绪队列移除 */
    {
        extern void sched_remove_ready(tcb_t *tcb);
        sched_remove_ready(current);
    }

    /* 设置阻塞状态 */
    current->state = TASK_STATE_BLOCKED;
    current->cont.result = KERN_OK;

    /* 设置超时唤醒时间 */
    current->cont.deadline = sched_timeout_deadline(timeout);

    irq_spin_unlock(&mqueue_lock, crit);

    /* 触发上下文切换 */
    hal_trigger_pendsv();

    /* 等待被唤醒 */
    while (current->state == TASK_STATE_BLOCKED) {
        __asm volatile("wfi");
        __asm volatile("dmb");
    }

    kern_err_t result = current->cont.result;

    if (result == KERN_OK) {
        crit = irq_spin_lock(&mqueue_lock);
        if (mq->count < mq->capacity) {
            mqueue_do_put(mq, msg);
        }
        irq_spin_unlock(&mqueue_lock, crit);
    } else {
        crit = irq_spin_lock(&mqueue_lock);
        if (current->cont.object == mq) {
            wait_queue_remove(&mq->send_queue, current);
            current->cont.object = NULL;
        }
        irq_spin_unlock(&mqueue_lock, crit);
    }

    return result;
}

#if SYSCALL_ENABLE
kern_err_t mqueue_send_syscall(queue_id_t queue_id, const void *msg,
                               uint32_t timeout) {
    uint32_t crit = irq_spin_lock(&mqueue_lock);

    mqueue_t *mq = get_mqueue(queue_id);
    if (mq == NULL || msg == NULL) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_PARAM;
    }

    if (mq->count < mq->capacity) {
        mqueue_do_put(mq, msg);
        mqueue_wake_recv_waiter(mq);
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_OK;
    }

    if (timeout == 0) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_TIMEOUT;
    }

    if (hal_irq_get_active() >= 0) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_ISR;
    }

    tcb_t *current = sched_get_current();
    if (current == NULL ||
        current->id < 0 || current->id >= KERNEL_MAX_TASKS) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_STATE;
    }

    memcpy(mqueue_syscall_send_msg[current->id], msg, mq->msg_size);

    syscall_cont_prepare_locked(BLOCK_REASON_QUEUE, mq);
    wait_queue_add(&mq->send_queue, current);

    irq_spin_unlock(&mqueue_lock, crit);
    return syscall_cont_commit(timeout);

    irq_spin_unlock(&mqueue_lock, crit);
    return KERN_SYSCALL_BLOCKED;
}
#endif

kern_err_t mqueue_trysend(queue_id_t queue_id, const void *msg) {
    uint32_t crit = irq_spin_lock(&mqueue_lock);

    mqueue_t *mq = get_mqueue(queue_id);
    if (mq == NULL) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_PARAM;
    }

    if (mq->count >= mq->capacity) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_BUSY;
    }

    mqueue_do_put(mq, msg);

    // 检查是否有任务在等待接收
    mqueue_wake_recv_waiter(mq);

    irq_spin_unlock(&mqueue_lock, crit);
    return KERN_OK;
}

kern_err_t mqueue_recv(queue_id_t queue_id, void *msg, uint32_t timeout) {
    uint32_t crit = irq_spin_lock(&mqueue_lock);

    mqueue_t *mq = get_mqueue(queue_id);
    if (mq == NULL) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_PARAM;
    }

    if (mq->count > 0) {
        mqueue_do_get(mq, msg);

        mqueue_wake_send_waiter(mq);

        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_OK;
    }

    if (timeout == 0) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_TIMEOUT;
    }

    if (hal_irq_get_active() >= 0) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_ISR;
    }

    tcb_t *current = sched_get_current();
    current->cont.op = BLOCK_REASON_QUEUE;
    current->cont.object = mq;

    /* 先加入等待队列 */
    wait_queue_add(&mq->recv_queue, current);

    /* 关键：在释放临界区前，完成状态转换
     * 1. 从就绪队列移除
     * 2. 设置状态为 BLOCKED
     * 这样当另一个任务调用 mqueue_trysend 时，sched_wakeup 能正确唤醒当前任务。
     */

    /* 从就绪队列移除 - 需要直接操作，因为我们在临界区内 */
    {
        extern void sched_remove_ready(tcb_t *tcb);
        sched_remove_ready(current);
    }

    /* 设置阻塞状态 */
    current->state = TASK_STATE_BLOCKED;
    current->cont.result = KERN_OK;

    /* 设置超时唤醒时间 */
    current->cont.deadline = sched_timeout_deadline(timeout);

    irq_spin_unlock(&mqueue_lock, crit);

    /* 触发上下文切换
     * 由于状态已经是 BLOCKED，PendSV 会选择下一个任务运行
     */
    hal_trigger_pendsv();

    /* 等待被唤醒 - 使用内存屏障确保正确读取状态 */
    while (current->state == TASK_STATE_BLOCKED) {
        __asm volatile("wfi");
        __asm volatile("dmb");
    }

    kern_err_t result = current->cont.result;

    if (result == KERN_OK) {
        crit = irq_spin_lock(&mqueue_lock);
        if (mq->count > 0) {
            mqueue_do_get(mq, msg);
        }
        irq_spin_unlock(&mqueue_lock, crit);
    } else {
        crit = irq_spin_lock(&mqueue_lock);
        if (current->cont.object == mq) {
            wait_queue_remove(&mq->recv_queue, current);
            current->cont.object = NULL;
        }
        irq_spin_unlock(&mqueue_lock, crit);
    }

    return result;
}

#if SYSCALL_ENABLE
kern_err_t mqueue_recv_syscall(queue_id_t queue_id, void *user_msg,
                               uint32_t timeout) {
    uint32_t crit = irq_spin_lock(&mqueue_lock);

    mqueue_t *mq = get_mqueue(queue_id);
    if (mq == NULL || user_msg == NULL) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_PARAM;
    }

    if (mq->count > 0) {
        mqueue_do_get(mq, user_msg);
        mqueue_wake_send_waiter(mq);
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_OK;
    }

    if (timeout == 0) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_TIMEOUT;
    }

    if (hal_irq_get_active() >= 0) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_ISR;
    }

    tcb_t *current = sched_get_current();
    if (current == NULL ||
        current->id < 0 || current->id >= KERNEL_MAX_TASKS) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_STATE;
    }

    current->cont.msg_buf = user_msg;
    syscall_cont_prepare_locked(BLOCK_REASON_QUEUE, mq);
    wait_queue_add(&mq->recv_queue, current);

    irq_spin_unlock(&mqueue_lock, crit);
    return syscall_cont_commit(timeout);
}
#endif

kern_err_t mqueue_tryrecv(queue_id_t queue_id, void *msg) {
    uint32_t crit = irq_spin_lock(&mqueue_lock);

    mqueue_t *mq = get_mqueue(queue_id);
    if (mq == NULL) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_PARAM;
    }

    if (mq->count == 0) {
        irq_spin_unlock(&mqueue_lock, crit);
        return KERN_ERR_BUSY;
    }

    mqueue_do_get(mq, msg);

    // 检查是否有任务在等待发送
    mqueue_wake_send_waiter(mq);

    irq_spin_unlock(&mqueue_lock, crit);
    return KERN_OK;
}

int32_t mqueue_get_count(queue_id_t queue_id) {
    uint32_t crit = irq_spin_lock(&mqueue_lock);

    mqueue_t *mq = get_mqueue(queue_id);
    if (mq == NULL) {
        irq_spin_unlock(&mqueue_lock, crit);
        return -1;
    }

    int32_t count = mq->count;

    irq_spin_unlock(&mqueue_lock, crit);
    return count;
}
