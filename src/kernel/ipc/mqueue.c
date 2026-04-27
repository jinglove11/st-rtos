/**
 * @file mqueue.c
 * @brief 消息队列实现
 */

#include "mqueue.h"
#include "scheduler.h"
#include "kernel_config.h"
#include "hal.h"
#include <string.h>

/*============================================================================
 * 静态分配的消息队列池和缓冲区
 *============================================================================*/

static mqueue_t mqueue_pool[KERN_MAX_MQUEUES];
static uint32_t mqueue_used_bitmap;

// 消息缓冲区池 (每个队列独立缓冲区)
static uint8_t mqueue_buffers[KERN_MAX_MQUEUES][KERN_MQUEUE_DEPTH * KERN_MSG_MAX_SIZE]
    __attribute__((aligned(4)));

/*============================================================================
 * 内部函数
 *============================================================================*/

// 分配消息队列 ID
static queue_id_t alloc_mqueue_id(void) {
    for (int i = 0; i < KERN_MAX_MQUEUES; i++) {
        if (!(mqueue_used_bitmap & (1U << i))) {
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

// 初始化等待队列
static void wait_queue_init(wait_queue_t *wq) {
    wq->head = NULL;
    wq->tail = NULL;
    wq->count = 0;
}

// 添加任务到等待队列尾部
static void wait_queue_add(wait_queue_t *wq, tcb_t *tcb) {
    tcb->next = NULL;
    tcb->prev = wq->tail;

    if (wq->tail) {
        wq->tail->next = tcb;
    } else {
        wq->head = tcb;
    }
    wq->tail = tcb;
    wq->count++;
}

// 从等待队列移除任务
static void wait_queue_remove(wait_queue_t *wq, tcb_t *tcb) {
    if (tcb->prev) {
        tcb->prev->next = tcb->next;
    } else {
        wq->head = tcb->next;
    }

    if (tcb->next) {
        tcb->next->prev = tcb->prev;
    } else {
        wq->tail = tcb->prev;
    }

    tcb->next = NULL;
    tcb->prev = NULL;
    wq->count--;
}

// 获取等待队列中最高优先级的任务
static tcb_t *wait_queue_get_highest(wait_queue_t *wq) {
    if (wq->head == NULL) {
        return NULL;
    }

    tcb_t *highest = wq->head;
    tcb_t *curr = wq->head->next;

    while (curr) {
        if (curr->priority < highest->priority) {
            highest = curr;
        }
        curr = curr->next;
    }

    return highest;
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

/*============================================================================
 * 公开接口实现
 *============================================================================*/

void mqueue_init(void) {
    memset(mqueue_pool, 0, sizeof(mqueue_pool));
    mqueue_used_bitmap = 0;
}

queue_id_t mqueue_create(uint32_t msg_size, uint32_t capacity) {
    uint32_t crit = hal_irq_save();

    queue_id_t id = alloc_mqueue_id();
    if (id == KERN_INVALID_ID) {
        hal_irq_restore(crit);
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

    mq->buffer = mqueue_buffers[id];
    mq->msg_size = (uint16_t)msg_size;
    mq->capacity = (uint16_t)capacity;
    mq->count = 0;
    mq->head = 0;
    mq->tail = 0;
    mq->in_use = 1;

    wait_queue_init(&mq->send_queue);
    wait_queue_init(&mq->recv_queue);

    hal_irq_restore(crit);
    return id;
}

kern_err_t mqueue_delete(queue_id_t queue_id) {
    uint32_t crit = hal_irq_save();

    mqueue_t *mq = get_mqueue(queue_id);
    if (mq == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    // 唤醒所有发送等待的任务
    tcb_t *tcb = mq->send_queue.head;
    while (tcb) {
        tcb_t *next = tcb->next;
        tcb->block_result = KERN_ERR_NOEXIST;
        sched_wakeup(tcb, KERN_ERR_NOEXIST);
        tcb = next;
    }

    // 唤醒所有接收等待的任务
    tcb = mq->recv_queue.head;
    while (tcb) {
        tcb_t *next = tcb->next;
        tcb->block_result = KERN_ERR_NOEXIST;
        sched_wakeup(tcb, KERN_ERR_NOEXIST);
        tcb = next;
    }

    // 清零并释放
    memset(mq, 0, sizeof(mqueue_t));
    free_mqueue_id(queue_id);

    hal_irq_restore(crit);
    return KERN_OK;
}

kern_err_t mqueue_send(queue_id_t queue_id, const void *msg, uint32_t timeout) {
    uint32_t crit = hal_irq_save();

    mqueue_t *mq = get_mqueue(queue_id);
    if (mq == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    if (mq->count < mq->capacity) {
        mqueue_do_put(mq, msg);

        if (mq->recv_queue.count > 0) {
            tcb_t *tcb = wait_queue_get_highest(&mq->recv_queue);
            if (tcb) {
                wait_queue_remove(&mq->recv_queue, tcb);
                tcb->block_result = KERN_OK;
                sched_wakeup(tcb, KERN_OK);
            }
        }

        hal_irq_restore(crit);
        return KERN_OK;
    }

    if (timeout == 0) {
        hal_irq_restore(crit);
        return KERN_ERR_TIMEOUT;
    }

    tcb_t *current = sched_get_current();
    current->block_reason = BLOCK_REASON_QUEUE;
    current->block_obj = mq;

    wait_queue_add(&mq->send_queue, current);

    hal_irq_restore(crit);

    kern_err_t result = sched_block(BLOCK_REASON_QUEUE, mq, timeout);

    if (result == KERN_OK) {
        crit = hal_irq_save();
        if (mq->count < mq->capacity) {
            mqueue_do_put(mq, msg);
        }
        hal_irq_restore(crit);
    } else {
        crit = hal_irq_save();
        if (current->block_obj == mq) {
            wait_queue_remove(&mq->send_queue, current);
            current->block_obj = NULL;
        }
        hal_irq_restore(crit);
    }

    return result;
}

kern_err_t mqueue_trysend(queue_id_t queue_id, const void *msg) {
    uint32_t crit = hal_irq_save();

    mqueue_t *mq = get_mqueue(queue_id);
    if (mq == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    if (mq->count >= mq->capacity) {
        hal_irq_restore(crit);
        return KERN_ERR_BUSY;
    }

    mqueue_do_put(mq, msg);

    // 检查是否有任务在等待接收
    if (mq->recv_queue.count > 0) {
        tcb_t *tcb = wait_queue_get_highest(&mq->recv_queue);
        if (tcb) {
            wait_queue_remove(&mq->recv_queue, tcb);
            tcb->block_result = KERN_OK;
            sched_wakeup(tcb, KERN_OK);
        }
    }

    hal_irq_restore(crit);
    return KERN_OK;
}

kern_err_t mqueue_recv(queue_id_t queue_id, void *msg, uint32_t timeout) {
    uint32_t crit = hal_irq_save();

    mqueue_t *mq = get_mqueue(queue_id);
    if (mq == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    if (mq->count > 0) {
        mqueue_do_get(mq, msg);

        if (mq->send_queue.count > 0) {
            tcb_t *tcb = wait_queue_get_highest(&mq->send_queue);
            if (tcb) {
                wait_queue_remove(&mq->send_queue, tcb);
                tcb->block_result = KERN_OK;
                sched_wakeup(tcb, KERN_OK);
            }
        }

        hal_irq_restore(crit);
        return KERN_OK;
    }

    if (timeout == 0) {
        hal_irq_restore(crit);
        return KERN_ERR_TIMEOUT;
    }

    tcb_t *current = sched_get_current();
    current->block_reason = BLOCK_REASON_QUEUE;
    current->block_obj = mq;

    wait_queue_add(&mq->recv_queue, current);

    hal_irq_restore(crit);

    kern_err_t result = sched_block(BLOCK_REASON_QUEUE, mq, timeout);

    if (result == KERN_OK) {
        crit = hal_irq_save();
        if (mq->count > 0) {
            mqueue_do_get(mq, msg);
        }
        hal_irq_restore(crit);
    } else {
        crit = hal_irq_save();
        if (current->block_obj == mq) {
            wait_queue_remove(&mq->recv_queue, current);
            current->block_obj = NULL;
        }
        hal_irq_restore(crit);
    }

    return result;
}

kern_err_t mqueue_tryrecv(queue_id_t queue_id, void *msg) {
    uint32_t crit = hal_irq_save();

    mqueue_t *mq = get_mqueue(queue_id);
    if (mq == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    if (mq->count == 0) {
        hal_irq_restore(crit);
        return KERN_ERR_BUSY;
    }

    mqueue_do_get(mq, msg);

    // 检查是否有任务在等待发送
    if (mq->send_queue.count > 0) {
        tcb_t *tcb = wait_queue_get_highest(&mq->send_queue);
        if (tcb) {
            wait_queue_remove(&mq->send_queue, tcb);
            tcb->block_result = KERN_OK;
            sched_wakeup(tcb, KERN_OK);
        }
    }

    hal_irq_restore(crit);
    return KERN_OK;
}

int32_t mqueue_get_count(queue_id_t queue_id) {
    uint32_t crit = hal_irq_save();

    mqueue_t *mq = get_mqueue(queue_id);
    if (mq == NULL) {
        hal_irq_restore(crit);
        return -1;
    }

    int32_t count = mq->count;

    hal_irq_restore(crit);
    return count;
}
