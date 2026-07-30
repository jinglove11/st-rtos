/**
 * @file semaphore.c
 * @brief 信号量实现
 */

#include "semaphore.h"
#include "wait_queue.h"
#include "scheduler.h"
#include "kernel_config.h"
#include "hal.h"
#include "spinlock.h"
#include "syscall.h"
#include "capability.h"
#include <string.h>

/*============================================================================
 * 静态分配的信号量池
 *============================================================================*/

static sem_t sem_pool[KERN_MAX_SEMAPHORES];
static uint32_t sem_used_bitmap;

/* M1: SMP 安全锁 (替代 hal_enter_critical,跨核互斥) */
static irq_spinlock_t sem_lock;

/*============================================================================
 * 内部函数
 *============================================================================*/

// 分配信号量 ID
static sem_id_t alloc_sem_id(void) {
    for (int i = 0; i < KERN_MAX_SEMAPHORES; i++) {
        if (!(sem_used_bitmap & (1U << i)) &&
            !kobj_generation_is_retired(sem_pool[i].hdr.generation)) {
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

/*============================================================================
 * 公开接口实现
 *============================================================================*/

void sem_init(void) {
    irq_spin_init_rank(&sem_lock, LOCKDEP_RANK_OBJECT);
    memset(sem_pool, 0, sizeof(sem_pool));
    sem_used_bitmap = 0;
}

sem_id_t sem_create(uint32_t initial_count, uint32_t max_count) {
    uint32_t crit = irq_spin_lock(&sem_lock);

    sem_id_t id = alloc_sem_id();
    if (id == KERN_INVALID_ID) {
        irq_spin_unlock(&sem_lock, crit);
        return KERN_INVALID_ID;
    }

    sem_t *sem = &sem_pool[id];

    // 如果 max_count 为 0, 表示无限制 (使用最大值)
    if (max_count == 0) {
        max_count = 0xFFFFFFFF;
    }

    /* M2-Step3a: 初始化对象 header (generation=1)。alloc_sem_id 复用 slot
     * 时,sem_pool[id] 已被 sem_delete memset 并写回 next generation,
     * 所以这里只在 hdr 未初始化时设 generation=1 (首次分配)。复用时
     * 保留 sem_delete 写入的 bumped generation。 */
    if (sem->hdr.generation == 0) {
        kobj_header_init(&sem->hdr, CAP_OBJ_SEMAPHORE);
    } else {
        sem->hdr.obj_type = CAP_OBJ_SEMAPHORE;
    }
    sem->count = initial_count;
    sem->max_count = max_count;
    sem->in_use = 1;
    wait_queue_init(&sem->wait_queue);

    irq_spin_unlock(&sem_lock, crit);
    return id;
}

kern_err_t sem_delete(sem_id_t sem_id) {
    uint32_t crit = irq_spin_lock(&sem_lock);

    sem_t *sem = sem_get(sem_id);
    if (sem == NULL) {
        irq_spin_unlock(&sem_lock, crit);
        return KERN_ERR_PARAM;
    }

    // 唤醒所有等待的任务
    tcb_t *tcb = sem->wait_queue.head;
    while (tcb) {
        tcb_t *next = tcb->wait_next;
        tcb->wait_next = NULL;
        tcb->wait_prev = NULL;
        tcb->cont.result = KERN_ERR_NOEXIST;  // 对象已删除
        sched_wakeup(tcb, KERN_ERR_NOEXIST);
        tcb = next;
    }

    // 清零并释放
#if CAP_ENABLE
    /* M2-Step1+3a: 撤销所有任务持有的指向此 sem 的 cap。M2-Step3a 改用
     * 真指针 &sem_pool[id] (与 sys_sem_create 的 cap_create_for_gen 一致),
     * 替代历史 (id+1) fake-pointer。 */
    (void)cap_revoke_object(sem, CAP_OBJ_SEMAPHORE);
#endif
    /* M2-Step3a: bump generation 并跨 memset 保留,使下次 alloc 拿到新 generation。
     * 即使 cap_revoke_object 漏撤某个 cap (例如 race),旧 cap 的 obj_generation
     * 与新对象的 hdr.generation 不匹配,cap_get_entry cross-check 拒绝。 */
    uint32_t next_gen = kobj_header_prepare_reuse(&sem->hdr);
    memset(sem, 0, sizeof(sem_t));
    sem->hdr.obj_type   = CAP_OBJ_SEMAPHORE;
    sem->hdr.generation = next_gen;
    free_sem_id(sem_id);

    irq_spin_unlock(&sem_lock, crit);
    return KERN_OK;
}

/* M2-Step3a: cap_resolve(CAP_OBJ_SEMAPHORE) 返回 &sem_pool[id] (header 在 offset 0)。
 * 本函数把 void* 还原为 sem_id 供 syscall 层调用 sem_*。 */
sem_id_t sem_id_from_obj(void *obj) {
    if (obj == NULL) {
        return KERN_INVALID_ID;
    }
    sem_t *sem = (sem_t *)obj;
    sem_id_t id = (sem_id_t)(sem - sem_pool);
    if (id < 0 || id >= KERN_MAX_SEMAPHORES) {
        return KERN_INVALID_ID;
    }
    return id;
}

/* M2-Step3a: 反向,sem_id → &sem_pool[id] 给 cap_create_for_gen 用。 */
void *sem_obj_for_cap(sem_id_t id) {
    if (id < 0 || id >= KERN_MAX_SEMAPHORES) {
        return NULL;
    }
    return (void *)&sem_pool[id];
}

void sem_cleanup_task(void *sem_obj, tcb_t *tcb) {
    sem_t *sem = (sem_t *)sem_obj;
    if (sem == NULL || tcb == NULL) return;

    uint32_t crit = irq_spin_lock(&sem_lock);
    if (sem >= &sem_pool[0] && sem < &sem_pool[KERN_MAX_SEMAPHORES] &&
        sem->in_use) {
        (void)wait_queue_remove_safe(&sem->wait_queue, tcb);
    }
    irq_spin_unlock(&sem_lock, crit);
}

kern_err_t sem_wait(sem_id_t sem_id, uint32_t timeout) {
    if (hal_irq_get_active() >= 0) {
        return KERN_ERR_ISR;
    }
    uint32_t crit = irq_spin_lock(&sem_lock);

    sem_t *sem = sem_get(sem_id);
    if (sem == NULL) {
        irq_spin_unlock(&sem_lock, crit);
        return KERN_ERR_PARAM;
    }

    if (sem->count > 0) {
        sem->count--;
        irq_spin_unlock(&sem_lock, crit);
        return KERN_OK;
    }

    if (timeout == 0) {
        irq_spin_unlock(&sem_lock, crit);
        return KERN_ERR_TIMEOUT;
    }

    tcb_t *current = sched_get_current();
    current->cont.op = BLOCK_REASON_SEM;
    current->cont.object = sem;

    wait_queue_add(&sem->wait_queue, current);

    /* 从就绪队列移除 */
    {
        extern void sched_remove_ready(tcb_t *tcb);
        sched_remove_ready(current);
    }

    /* 设置阻塞状态 */
    current->state = TASK_STATE_BLOCKED;
    current->cont.result = KERN_OK;

    current->cont.deadline = sched_timeout_deadline(timeout);

    irq_spin_unlock(&sem_lock, crit);

    /* 触发上下文切换 */
    hal_trigger_pendsv();

    /* 等待被唤醒 */
    while (current->state == TASK_STATE_BLOCKED) {
        __asm volatile("wfi");
        __asm volatile("dmb");
    }

    kern_err_t result = current->cont.result;

    crit = irq_spin_lock(&sem_lock);
    if (result != KERN_OK) {
        if (current->cont.object == sem) {
            wait_queue_remove(&sem->wait_queue, current);
            current->cont.object = NULL;
        }
    }
    irq_spin_unlock(&sem_lock, crit);

    return result;
}

#if SYSCALL_ENABLE
kern_err_t sem_wait_syscall(sem_id_t sem_id, uint32_t timeout) {
    if (hal_irq_get_active() >= 0) {
        return KERN_ERR_ISR;
    }

    uint32_t crit = irq_spin_lock(&sem_lock);

    sem_t *sem = sem_get(sem_id);
    if (sem == NULL) {
        irq_spin_unlock(&sem_lock, crit);
        return KERN_ERR_PARAM;
    }

    if (sem->count > 0) {
        sem->count--;
        irq_spin_unlock(&sem_lock, crit);
        return KERN_OK;
    }

    if (timeout == 0) {
        irq_spin_unlock(&sem_lock, crit);
        return KERN_ERR_TIMEOUT;
    }

    tcb_t *current = sched_get_current();
    if (current == NULL) {
        irq_spin_unlock(&sem_lock, crit);
        return KERN_ERR_STATE;
    }

    current->cont.active = 1;
    current->cont.op = BLOCK_REASON_SEM;
    current->cont.object = sem;
    current->cont.result = KERN_OK;
    wait_queue_add(&sem->wait_queue, current);

    sched_remove_ready(current);
    current->state = TASK_STATE_BLOCKED;

    current->cont.deadline = sched_timeout_deadline(timeout);

    irq_spin_unlock(&sem_lock, crit);
    return KERN_SYSCALL_BLOCKED;
}
#endif

kern_err_t sem_trywait(sem_id_t sem_id) {
    uint32_t crit = irq_spin_lock(&sem_lock);

    sem_t *sem = sem_get(sem_id);
    if (sem == NULL) {
        irq_spin_unlock(&sem_lock, crit);
        return KERN_ERR_PARAM;
    }

    if (sem->count > 0) {
        sem->count--;
        irq_spin_unlock(&sem_lock, crit);
        return KERN_OK;
    }

    irq_spin_unlock(&sem_lock, crit);
    return KERN_ERR_BUSY;
}

kern_err_t sem_post(sem_id_t sem_id) {
    uint32_t crit = irq_spin_lock(&sem_lock);

    sem_t *sem = sem_get(sem_id);
    if (sem == NULL) {
        irq_spin_unlock(&sem_lock, crit);
        return KERN_ERR_PARAM;
    }

    // 检查是否有任务在等待
    if (sem->wait_queue.count > 0) {
        // 唤醒最高优先级的等待任务
        tcb_t *tcb = wait_queue_get_highest(&sem->wait_queue);
        if (tcb) {
            wait_queue_remove(&sem->wait_queue, tcb);
            tcb->cont.result = KERN_OK;
            sched_wakeup(tcb, KERN_OK);
        }
    } else {
        // 没有等待任务, 增加计数
        if (sem->count < sem->max_count) {
            sem->count++;
        } else {
            irq_spin_unlock(&sem_lock, crit);
            return KERN_ERR_OVERFLOW;  // 计数溢出
        }
    }

    irq_spin_unlock(&sem_lock, crit);
    return KERN_OK;
}

int32_t sem_get_count(sem_id_t sem_id) {
    uint32_t crit = irq_spin_lock(&sem_lock);

    sem_t *sem = sem_get(sem_id);
    if (sem == NULL) {
        irq_spin_unlock(&sem_lock, crit);
        return -1;
    }

    int32_t count = (int32_t)sem->count;

    irq_spin_unlock(&sem_lock, crit);
    return count;
}
