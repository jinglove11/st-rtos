/**
 * @file mutex.c
 * @brief 互斥锁实现 (含优先级继承)
 */

#include "mutex.h"
#include "wait_queue.h"
#include "scheduler.h"
#include "kernel_config.h"
#include "hal.h"
#include "syscall.h"
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

#if KERN_MUTEX_PI
// 优先级继承: 提升持有者优先级
static void mutex_priority_inherit(mutex_t *mutex, tcb_t *waiter) {
    // 获取锁持有者的 TCB
    if (mutex->owner < 0) return;

    extern tcb_t *task_get_tcb(task_id_t task_id);
    tcb_t *owner = task_get_tcb(mutex->owner);
    if (owner == NULL) return;

    // 如果等待者优先级高于持有者, 提升持有者优先级
    if (waiter->priority < owner->priority) {
        owner->priority = waiter->priority;

        // 如果持有者在就绪队列中, 需要重新插入以更新位图
        if (owner->state == TASK_STATE_READY) {
            extern void sched_reinsert_by_priority(tcb_t *tcb);
            sched_reinsert_by_priority(owner);
        }
        // 如果持有者是 RUNNING 状态，优先级已经提升
        // 当高优先级任务阻塞后，调度器会根据优先级选择下一个任务
        // 但持有者不在就绪队列中，需要特殊处理
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
 * 死锁检测
 *============================================================================*/

#if MUTEX_DEADLOCK_DETECT

/**
 * @brief 检测等待图是否存在环（死锁）
 *
 * 从互斥锁的当前持有者出发，沿阻塞链遍历：
 * 持有者 -> 持有者等待的锁 -> 下一持有者 -> ...
 * 若链回到 caller 则存在死锁。
 *
 * @param mutex  将要等待的互斥锁
 * @param caller 尝试获取锁的调用任务
 * @return true  将形成死锁
 * @return false 安全
 *
 * @note 必须在临界区内调用
 */
static bool mutex_would_deadlock(mutex_t *mutex, tcb_t *caller)
{
    task_id_t current_owner = mutex->owner;
    int       depth;

    for (depth = 0; depth < KERN_MAX_TASKS; depth++) {
        if (current_owner < 0) {
            return false;
        }

        if (current_owner == caller->id) {
            return true;
        }

        tcb_t *owner_tcb = sched_get_tcb(current_owner);
        if (owner_tcb == NULL) {
            return false;
        }

        if (owner_tcb->state != TASK_STATE_BLOCKED) {
            return false;
        }

        if (owner_tcb->block_reason != BLOCK_REASON_MUTEX) {
            return false;
        }

        if (owner_tcb->block_obj == NULL) {
            return false;
        }

        mutex_t *owner_mutex = (mutex_t *)owner_tcb->block_obj;
        current_owner = owner_mutex->owner;
    }

    return false;  /* 深度保护 */
}

#endif /* MUTEX_DEADLOCK_DETECT */

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
        tcb_t *next = tcb->wait_next;
        tcb->wait_next = NULL;
        tcb->wait_prev = NULL;
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
    uint32_t crit = hal_enter_critical();

    mutex_t *mutex = mutex_get(mutex_id);
    if (mutex == NULL) {
        hal_exit_critical(crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();

    if (mutex->owner == KERN_INVALID_ID) {
        mutex->owner = current->id;
        mutex->lock_count = 1;
        mutex->owner_original_prio = current->priority;
        hal_exit_critical(crit);
        return KERN_OK;
    }

    if (mutex->owner == current->id) {
        mutex->lock_count++;
        hal_exit_critical(crit);
        return KERN_OK;
    }

    if (timeout == 0) {
        hal_exit_critical(crit);
        return KERN_ERR_TIMEOUT;
    }

    if (hal_irq_get_active() >= 0) {
        hal_exit_critical(crit);
        return KERN_ERR_ISR;
    }

#if MUTEX_DEADLOCK_DETECT
    if (mutex_would_deadlock(mutex, current)) {
        hal_exit_critical(crit);
        return KERN_ERR_DEADLOCK;
    }
#endif

#if KERN_MUTEX_PI
    mutex_priority_inherit(mutex, current);

    // 如果持有锁的任务正在运行，将其加入就绪队列
    // 这样当高优先级任务阻塞后，持有锁的任务能被调度器选中
    extern tcb_t *task_get_tcb(task_id_t task_id);
    tcb_t *owner = task_get_tcb(mutex->owner);
    if (owner && owner->state == TASK_STATE_RUNNING) {
        extern void sched_add_ready(tcb_t *tcb);
        owner->state = TASK_STATE_READY;
        sched_add_ready(owner);
    }
#endif

    current->block_reason = BLOCK_REASON_MUTEX;
    current->block_obj = mutex;

    wait_queue_add(&mutex->wait_queue, current);

    /* 从就绪队列移除 */
    {
        extern void sched_remove_ready(tcb_t *tcb);
        sched_remove_ready(current);
    }

    /* 设置阻塞状态 */
    current->state = TASK_STATE_BLOCKED;
    current->block_result = KERN_OK;

    /* 设置超时唤醒时间 */
    if (timeout > 0) {
        extern uint32_t sched_get_tick_count(void);
        current->wake_tick = sched_get_tick_count() + timeout;
    } else {
        current->wake_tick = 0;
    }

    hal_exit_critical(crit);

    /* 触发上下文切换 */
    hal_trigger_pendsv();

    /* 等待被唤醒 */
    while (current->state == TASK_STATE_BLOCKED) {
        __asm volatile("wfi");
        __asm volatile("dmb");
    }

    kern_err_t result = current->block_result;

    crit = hal_enter_critical();
    if (result != KERN_OK) {
        if (current->block_obj == mutex) {
            wait_queue_remove(&mutex->wait_queue, current);
            current->block_obj = NULL;
        }
    }
    hal_exit_critical(crit);

    return result;
}

#if SYSCALL_ENABLE
kern_err_t mutex_lock_syscall(mutex_id_t mutex_id, uint32_t timeout) {
    uint32_t crit = hal_enter_critical();

    mutex_t *mutex = mutex_get(mutex_id);
    if (mutex == NULL) {
        hal_exit_critical(crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();
    if (current == NULL) {
        hal_exit_critical(crit);
        return KERN_ERR_STATE;
    }

    if (mutex->owner == KERN_INVALID_ID) {
        mutex->owner = current->id;
        mutex->lock_count = 1;
        mutex->owner_original_prio = current->priority;
        hal_exit_critical(crit);
        return KERN_OK;
    }

    if (mutex->owner == current->id) {
        mutex->lock_count++;
        hal_exit_critical(crit);
        return KERN_OK;
    }

    if (timeout == 0) {
        hal_exit_critical(crit);
        return KERN_ERR_TIMEOUT;
    }

    if (hal_irq_get_active() >= 0) {
        hal_exit_critical(crit);
        return KERN_ERR_ISR;
    }

#if MUTEX_DEADLOCK_DETECT
    if (mutex_would_deadlock(mutex, current)) {
        hal_exit_critical(crit);
        return KERN_ERR_DEADLOCK;
    }
#endif

#if KERN_MUTEX_PI
    mutex_priority_inherit(mutex, current);

    extern tcb_t *task_get_tcb(task_id_t task_id);
    tcb_t *owner = task_get_tcb(mutex->owner);
    if (owner && owner->state == TASK_STATE_RUNNING) {
        extern void sched_add_ready(tcb_t *tcb);
        owner->state = TASK_STATE_READY;
        sched_add_ready(owner);
    }
#endif

    current->syscall_blocked = 1;
    current->block_reason = BLOCK_REASON_MUTEX;
    current->block_obj = mutex;
    current->block_result = KERN_OK;
    wait_queue_add(&mutex->wait_queue, current);

    sched_remove_ready(current);
    current->state = TASK_STATE_BLOCKED;

    if (timeout > 0) {
        current->wake_tick = sched_get_tick_count() + timeout;
    } else {
        current->wake_tick = 0;
    }

    hal_exit_critical(crit);
    return KERN_SYSCALL_BLOCKED;
}
#endif

kern_err_t mutex_trylock(mutex_id_t mutex_id) {
    uint32_t crit = hal_enter_critical();

    mutex_t *mutex = mutex_get(mutex_id);
    if (mutex == NULL) {
        hal_exit_critical(crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();

    // 如果未被持有, 直接获取
    if (mutex->owner == KERN_INVALID_ID) {
        mutex->owner = current->id;
        mutex->lock_count = 1;
        mutex->owner_original_prio = current->priority;
        hal_exit_critical(crit);
        return KERN_OK;
    }

    // 如果当前任务已持有, 递归锁
    if (mutex->owner == current->id) {
        mutex->lock_count++;
        hal_exit_critical(crit);
        return KERN_OK;
    }

    hal_exit_critical(crit);
    return KERN_ERR_BUSY;
}

kern_err_t mutex_unlock(mutex_id_t mutex_id) {
    uint32_t crit = hal_enter_critical();

    mutex_t *mutex = mutex_get(mutex_id);
    if (mutex == NULL) {
        hal_exit_critical(crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();

    // 检查是否是持有者
    if (mutex->owner != current->id) {
        hal_exit_critical(crit);
        return KERN_ERR_STATE;
    }

    // 递归解锁
    mutex->lock_count--;
    if (mutex->lock_count > 0) {
        hal_exit_critical(crit);
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

    hal_exit_critical(crit);
    return KERN_OK;
}

/*============================================================================
 * 死锁诊断 API
 *============================================================================*/

#if MUTEX_DEADLOCK_DETECT

int mutex_deadlock_check(void)
{
    uint32_t crit = hal_enter_critical();
    int      deadlocked_count = 0;

    for (task_id_t id = 0; id < KERN_MAX_TASKS; id++) {
        tcb_t *tcb = sched_get_tcb(id);
        if (tcb == NULL) {
            continue;
        }

        if (tcb->state != TASK_STATE_BLOCKED) {
            continue;
        }
        if (tcb->block_reason != BLOCK_REASON_MUTEX) {
            continue;
        }
        if (tcb->block_obj == NULL) {
            continue;
        }

        mutex_t *blocking_mutex = (mutex_t *)tcb->block_obj;
        if (mutex_would_deadlock(blocking_mutex, tcb)) {
            deadlocked_count++;
        }
    }

    hal_exit_critical(crit);
    return deadlocked_count;
}

#endif /* MUTEX_DEADLOCK_DETECT */
