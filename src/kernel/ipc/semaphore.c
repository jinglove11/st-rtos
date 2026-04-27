/**
 * @file semaphore.c
 * @brief 信号量实现
 */

#include "semaphore.h"
#include "scheduler.h"
#include "kernel_config.h"
#include "hal.h"
#include <string.h>

/*============================================================================
 * 静态分配的信号量池
 *============================================================================*/

static sem_t sem_pool[KERN_MAX_SEMAPHORES];
static uint32_t sem_used_bitmap;

/*============================================================================
 * 内部函数
 *============================================================================*/

// 分配信号量 ID
static sem_id_t alloc_sem_id(void) {
    for (int i = 0; i < KERN_MAX_SEMAPHORES; i++) {
        if (!(sem_used_bitmap & (1U << i))) {
            sem_used_bitmap |= (1U << i);
            return (sem_id_t)i;
        }
    }
    return KERN_INVALID_ID;
}

// 释放信号量 ID
static void free_sem_id(sem_id_t id) {
    if (id >= 0 && id < KERN_MAX_SEMAPHORES) {
        sem_used_bitmap &= ~(1U << id);
    }
}

// 获取信号量指针
static sem_t *sem_get(sem_id_t id) {
    if (id < 0 || id >= KERN_MAX_SEMAPHORES) {
        return NULL;
    }
    if (!sem_pool[id].in_use) {
        return NULL;
    }
    return &sem_pool[id];
}

// 初始化等待队列
static void wait_queue_init(wait_queue_t *wq) {
    wq->head = NULL;
    wq->tail = NULL;
    wq->count = 0;
}

// 添加任务到等待队列尾部 (按优先级排序)
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

/*============================================================================
 * 公开接口实现
 *============================================================================*/

void sem_init(void) {
    memset(sem_pool, 0, sizeof(sem_pool));
    sem_used_bitmap = 0;
}

sem_id_t sem_create(uint32_t initial_count, uint32_t max_count) {
    uint32_t crit = hal_irq_save();

    sem_id_t id = alloc_sem_id();
    if (id == KERN_INVALID_ID) {
        hal_irq_restore(crit);
        return KERN_INVALID_ID;
    }

    sem_t *sem = &sem_pool[id];

    // 如果 max_count 为 0, 表示无限制 (使用最大值)
    if (max_count == 0) {
        max_count = 0xFFFFFFFF;
    }

    sem->count = initial_count;
    sem->max_count = max_count;
    sem->in_use = 1;
    wait_queue_init(&sem->wait_queue);

    hal_irq_restore(crit);
    return id;
}

kern_err_t sem_delete(sem_id_t sem_id) {
    uint32_t crit = hal_irq_save();

    sem_t *sem = sem_get(sem_id);
    if (sem == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    // 唤醒所有等待的任务
    tcb_t *tcb = sem->wait_queue.head;
    while (tcb) {
        tcb_t *next = tcb->next;
        tcb->block_result = KERN_ERR_NOEXIST;  // 对象已删除
        sched_wakeup(tcb, KERN_ERR_NOEXIST);
        tcb = next;
    }

    // 清零并释放
    memset(sem, 0, sizeof(sem_t));
    free_sem_id(sem_id);

    hal_irq_restore(crit);
    return KERN_OK;
}

kern_err_t sem_wait(sem_id_t sem_id, uint32_t timeout) {
    uint32_t crit = hal_irq_save();

    sem_t *sem = sem_get(sem_id);
    if (sem == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    if (sem->count > 0) {
        sem->count--;
        hal_irq_restore(crit);
        return KERN_OK;
    }

    if (timeout == 0) {
        hal_irq_restore(crit);
        return KERN_ERR_TIMEOUT;
    }

    tcb_t *current = sched_get_current();
    current->block_reason = BLOCK_REASON_SEM;
    current->block_obj = sem;

    wait_queue_add(&sem->wait_queue, current);

    hal_irq_restore(crit);

    kern_err_t result = sched_block(BLOCK_REASON_SEM, sem, timeout);

    crit = hal_irq_save();
    if (result != KERN_OK) {
        if (current->block_obj == sem) {
            wait_queue_remove(&sem->wait_queue, current);
            current->block_obj = NULL;
        }
    }
    hal_irq_restore(crit);

    return result;
}

kern_err_t sem_trywait(sem_id_t sem_id) {
    uint32_t crit = hal_irq_save();

    sem_t *sem = sem_get(sem_id);
    if (sem == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    if (sem->count > 0) {
        sem->count--;
        hal_irq_restore(crit);
        return KERN_OK;
    }

    hal_irq_restore(crit);
    return KERN_ERR_BUSY;
}

kern_err_t sem_post(sem_id_t sem_id) {
    uint32_t crit = hal_irq_save();

    sem_t *sem = sem_get(sem_id);
    if (sem == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    // 检查是否有任务在等待
    if (sem->wait_queue.count > 0) {
        // 唤醒最高优先级的等待任务
        tcb_t *tcb = wait_queue_get_highest(&sem->wait_queue);
        if (tcb) {
            wait_queue_remove(&sem->wait_queue, tcb);
            tcb->block_result = KERN_OK;
            sched_wakeup(tcb, KERN_OK);
        }
    } else {
        // 没有等待任务, 增加计数
        if (sem->count < sem->max_count) {
            sem->count++;
        } else {
            hal_irq_restore(crit);
            return KERN_ERR_OVERFLOW;  // 计数溢出
        }
    }

    hal_irq_restore(crit);
    return KERN_OK;
}

int32_t sem_get_count(sem_id_t sem_id) {
    uint32_t crit = hal_irq_save();

    sem_t *sem = sem_get(sem_id);
    if (sem == NULL) {
        hal_irq_restore(crit);
        return -1;
    }

    int32_t count = (int32_t)sem->count;

    hal_irq_restore(crit);
    return count;
}
