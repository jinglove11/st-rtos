/**
 * @file mutex.c
 * @brief 互斥锁实现 (含优先级继承)
 */

#include "mutex.h"
#include "scheduler.h"
#include "kernel_config.h"
#include "hal.h"
#include <string.h>

/*============================================================================
 * 静态分配的互斥锁池
 *============================================================================*/

static mutex_t mutex_pool[KERN_MAX_MUTEXES];
static uint32_t mutex_used_bitmap;

/*============================================================================
 * 内部函数
 *============================================================================*/

// 分配互斥锁 ID
static mutex_id_t alloc_mutex_id(void) {
    for (int i = 0; i < KERN_MAX_MUTEXES; i++) {
        if (!(mutex_used_bitmap & (1U << i))) {
            mutex_used_bitmap |= (1U << i);
            return (mutex_id_t)i;
        }
    }
    return KERN_INVALID_ID;
}

// 释放互斥锁 ID
static void free_mutex_id(mutex_id_t id) {
    if (id >= 0 && id < KERN_MAX_MUTEXES) {
        mutex_used_bitmap &= ~(1U << id);
    }
}

// 获取互斥锁指针
static mutex_t *mutex_get(mutex_id_t id) {
    if (id < 0 || id >= KERN_MAX_MUTEXES) {
        return NULL;
    }
    if (!mutex_pool[id].in_use) {
        return NULL;
    }
    return &mutex_pool[id];
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

#if KERN_MUTEX_PI
// 优先级继承: 提升持有者优先级
static void mutex_priority_inherit(mutex_t *mutex, tcb_t *waiter) {
    (void)mutex;  // 未使用, 但保留参数以便将来扩展
    tcb_t *owner = sched_get_current();
    if (owner == NULL) return;

    // 如果等待者优先级高于持有者, 提升持有者优先级
    if (waiter->priority < owner->priority) {
        owner->priority = waiter->priority;
    }
}

// 优先级继承: 恢复持有者优先级
static void mutex_priority_uninherit(mutex_t *mutex) {
    tcb_t *owner = sched_get_current();
    if (owner == NULL) return;

    // 检查是否还有更高优先级的等待者
    if (mutex->wait_queue.count > 0) {
        tcb_t *highest = wait_queue_get_highest(&mutex->wait_queue);
        if (highest && highest->priority < owner->base_priority) {
            owner->priority = highest->priority;
            return;
        }
    }

    // 恢复原始优先级
    owner->priority = owner->base_priority;
}
#else
// 无优先级继承时的空实现
static void mutex_priority_inherit(mutex_t *mutex, tcb_t *waiter) {
    (void)mutex;
    (void)waiter;
}
static void mutex_priority_uninherit(mutex_t *mutex) {
    (void)mutex;
}
#endif

/*============================================================================
 * 公开接口实现
 *============================================================================*/

void mutex_init(void) {
    memset(mutex_pool, 0, sizeof(mutex_pool));
    mutex_used_bitmap = 0;
}

mutex_id_t mutex_create(void) {
    uint32_t crit = hal_irq_save();

    mutex_id_t id = alloc_mutex_id();
    if (id == KERN_INVALID_ID) {
        hal_irq_restore(crit);
        return KERN_INVALID_ID;
    }

    mutex_t *mutex = &mutex_pool[id];
    mutex->owner = KERN_INVALID_ID;
    mutex->lock_count = 0;
    mutex->owner_original_prio = 0;
    mutex->in_use = 1;
    wait_queue_init(&mutex->wait_queue);

    hal_irq_restore(crit);
    return id;
}

kern_err_t mutex_delete(mutex_id_t mutex_id) {
    uint32_t crit = hal_irq_save();

    mutex_t *mutex = mutex_get(mutex_id);
    if (mutex == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    // 如果互斥锁被持有, 不能删除
    if (mutex->owner != KERN_INVALID_ID) {
        hal_irq_restore(crit);
        return KERN_ERR_BUSY;
    }

    // 唤醒所有等待的任务
    tcb_t *tcb = mutex->wait_queue.head;
    while (tcb) {
        tcb_t *next = tcb->next;
        tcb->block_result = KERN_ERR_NOEXIST;
        sched_wakeup(tcb, KERN_ERR_NOEXIST);
        tcb = next;
    }

    // 清零并释放
    memset(mutex, 0, sizeof(mutex_t));
    free_mutex_id(mutex_id);

    hal_irq_restore(crit);
    return KERN_OK;
}

kern_err_t mutex_lock(mutex_id_t mutex_id, uint32_t timeout) {
    uint32_t crit = hal_irq_save();

    mutex_t *mutex = mutex_get(mutex_id);
    if (mutex == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();

    if (mutex->owner == KERN_INVALID_ID) {
        mutex->owner = current->id;
        mutex->lock_count = 1;
        mutex->owner_original_prio = current->priority;
        hal_irq_restore(crit);
        return KERN_OK;
    }

    if (mutex->owner == current->id) {
        mutex->lock_count++;
        hal_irq_restore(crit);
        return KERN_OK;
    }

    if (timeout == 0) {
        hal_irq_restore(crit);
        return KERN_ERR_TIMEOUT;
    }

#if KERN_MUTEX_PI
    mutex_priority_inherit(mutex, current);
#endif

    current->block_reason = BLOCK_REASON_MUTEX;
    current->block_obj = mutex;

    wait_queue_add(&mutex->wait_queue, current);

    hal_irq_restore(crit);

    kern_err_t result = sched_block(BLOCK_REASON_MUTEX, mutex, timeout);

    crit = hal_irq_save();
    if (result != KERN_OK) {
        if (current->block_obj == mutex) {
            wait_queue_remove(&mutex->wait_queue, current);
            current->block_obj = NULL;
        }
    }
    hal_irq_restore(crit);

    return result;
}

kern_err_t mutex_trylock(mutex_id_t mutex_id) {
    uint32_t crit = hal_irq_save();

    mutex_t *mutex = mutex_get(mutex_id);
    if (mutex == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();

    // 如果未被持有, 直接获取
    if (mutex->owner == KERN_INVALID_ID) {
        mutex->owner = current->id;
        mutex->lock_count = 1;
        mutex->owner_original_prio = current->priority;
        hal_irq_restore(crit);
        return KERN_OK;
    }

    // 如果当前任务已持有, 递归锁
    if (mutex->owner == current->id) {
        mutex->lock_count++;
        hal_irq_restore(crit);
        return KERN_OK;
    }

    hal_irq_restore(crit);
    return KERN_ERR_BUSY;
}

kern_err_t mutex_unlock(mutex_id_t mutex_id) {
    uint32_t crit = hal_irq_save();

    mutex_t *mutex = mutex_get(mutex_id);
    if (mutex == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();

    // 检查是否是持有者
    if (mutex->owner != current->id) {
        hal_irq_restore(crit);
        return KERN_ERR_STATE;
    }

    // 递归解锁
    mutex->lock_count--;
    if (mutex->lock_count > 0) {
        hal_irq_restore(crit);
        return KERN_OK;
    }

#if KERN_MUTEX_PI
    // 恢复优先级
    mutex_priority_uninherit(mutex);
#endif

    // 检查是否有任务在等待
    if (mutex->wait_queue.count > 0) {
        // 唤醒最高优先级的等待任务
        tcb_t *tcb = wait_queue_get_highest(&mutex->wait_queue);
        if (tcb) {
            wait_queue_remove(&mutex->wait_queue, tcb);

            // 转移所有权
            mutex->owner = tcb->id;
            mutex->lock_count = 1;
            mutex->owner_original_prio = tcb->priority;

            tcb->block_result = KERN_OK;
            sched_wakeup(tcb, KERN_OK);
        }
    } else {
        // 没有等待任务, 释放互斥锁
        mutex->owner = KERN_INVALID_ID;
    }

    hal_irq_restore(crit);
    return KERN_OK;
}
