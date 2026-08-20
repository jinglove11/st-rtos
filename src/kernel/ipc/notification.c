/**
 * @file notification.c
 * @brief P1-1 (A4): 独立 notification 对象实现 — seL4 风格单字聚合通知
 *
 * 结构照抄 event.c 的对象池/header/等待队列模式;语义差异见 notification.h。
 * 阻塞路径走 M3-Task3 两阶段 continuation 协议(prepare_locked + commit),
 * 唤醒侧(signal)在持有对象锁时完成 word 移交与 copy_to_user,唤醒后
 * 等待者无需(也不能)重读对象。
 */

#include "notification.h"
#include "wait_queue.h"
#include "scheduler.h"
#include "kernel_config.h"
#include "hal.h"
#include "spinlock.h"
#include "syscall.h"
#include "continuation.h"
#include "capability.h"
#include "usercopy.h"
#include <string.h>

#if IPC_NOTIFICATION

/*============================================================================
 * 静态池
 *============================================================================*/

static notification_t notification_pool[KERN_MAX_NOTIFICATIONS];
static uint32_t notification_used_bitmap;
static irq_spinlock_t notification_lock;

/*============================================================================
 * 内部函数
 *============================================================================*/

static notification_id_t alloc_notification_id(void) {
    for (int i = 0; i < KERN_MAX_NOTIFICATIONS; i++) {
        if (!(notification_used_bitmap & (1U << i)) &&
            !kobj_generation_is_retired(notification_pool[i].hdr.generation)) {
            notification_used_bitmap |= (1U << i);
            return (notification_id_t)i;
        }
    }
    return KERN_INVALID_ID;
}

static void free_notification_id(notification_id_t id) {
    if (id >= 0 && id < KERN_MAX_NOTIFICATIONS) {
        notification_used_bitmap &= ~(1U << id);
    }
}

static notification_t *get_notification(notification_id_t id) {
    if (id < 0 || id >= KERN_MAX_NOTIFICATIONS) {
        return NULL;
    }
    if (!notification_pool[id].in_use) {
        return NULL;
    }
    return &notification_pool[id];
}

/*============================================================================
 * 生命周期
 *============================================================================*/

void notification_init(void) {
    irq_spin_init_rank(&notification_lock, LOCKDEP_RANK_OBJECT);
    memset(notification_pool, 0, sizeof(notification_pool));
    notification_used_bitmap = 0;
}

notification_id_t notification_create(void) {
    uint32_t crit = irq_spin_lock(&notification_lock);

    notification_id_t id = alloc_notification_id();
    if (id == KERN_INVALID_ID) {
        irq_spin_unlock(&notification_lock, crit);
        return KERN_INVALID_ID;
    }

    notification_t *ntfn = &notification_pool[id];
    if (ntfn->hdr.generation == 0) {
        kobj_header_init(&ntfn->hdr, CAP_OBJ_NOTIFICATION);
    } else {
        ntfn->hdr.obj_type = CAP_OBJ_NOTIFICATION;
    }
    ntfn->word = 0;
    ntfn->in_use = 1;
    wait_queue_init(&ntfn->wait_queue);

    irq_spin_unlock(&notification_lock, crit);
    return id;
}

kern_err_t notification_delete(notification_id_t id) {
    uint32_t crit = irq_spin_lock(&notification_lock);

    notification_t *ntfn = get_notification(id);
    if (ntfn == NULL) {
        irq_spin_unlock(&notification_lock, crit);
        return KERN_ERR_PARAM;
    }

    /* 唤醒所有等待者(整字移交已无意义,对象将不复存在) */
    tcb_t *tcb = ntfn->wait_queue.head;
    while (tcb) {
        tcb_t *next = tcb->wait_next;
        tcb->wait_next = NULL;
        tcb->wait_prev = NULL;
        tcb->cont.object = NULL;
        tcb->cont.result = KERN_ERR_NOEXIST;
        syscall_cont_wake(tcb, KERN_ERR_NOEXIST);
        tcb = next;
    }

#if CAP_ENABLE
    (void)cap_revoke_object(ntfn, CAP_OBJ_NOTIFICATION);
#endif
    uint32_t next_gen = kobj_header_prepare_reuse(&ntfn->hdr);
    memset(ntfn, 0, sizeof(notification_t));
    ntfn->hdr.obj_type   = CAP_OBJ_NOTIFICATION;
    ntfn->hdr.generation = next_gen;
    free_notification_id(id);

    irq_spin_unlock(&notification_lock, crit);
    return KERN_OK;
}

/*============================================================================
 * cap 集成
 *============================================================================*/

notification_id_t notification_id_from_obj(void *obj) {
    if (obj == NULL) return KERN_INVALID_ID;
    notification_t *ntfn = (notification_t *)obj;
    notification_id_t id =
        (notification_id_t)(ntfn - &notification_pool[0]);
    if (id < 0 || id >= KERN_MAX_NOTIFICATIONS) return KERN_INVALID_ID;
    return id;
}

void *notification_obj_for_cap(notification_id_t id) {
    if (id < 0 || id >= KERN_MAX_NOTIFICATIONS) return NULL;
    return (void *)&notification_pool[id];
}

void notification_cleanup_task(void *ntfn_obj, tcb_t *tcb) {
    notification_t *ntfn = (notification_t *)ntfn_obj;
    if (ntfn == NULL || tcb == NULL) return;

    uint32_t crit = irq_spin_lock(&notification_lock);
    if (ntfn >= &notification_pool[0] &&
        ntfn < &notification_pool[KERN_MAX_NOTIFICATIONS] &&
        ntfn->in_use) {
        (void)wait_queue_remove_safe(&ntfn->wait_queue, tcb);
    }
    irq_spin_unlock(&notification_lock, crit);
}

/*============================================================================
 * signal: 聚合 + 至多唤醒一个等待者并移交整字
 *============================================================================*/

static kern_err_t ntfn_signal_locked(notification_t *ntfn, uint32_t badge,
                                     uint32_t crit) {
    ntfn->word |= badge;

    if (ntfn->word != 0U) {
        tcb_t *waiter = ntfn->wait_queue.head;
        if (waiter != NULL) {
            uint32_t consumed = ntfn->word;
            ntfn->word = 0;

            wait_queue_remove(&ntfn->wait_queue, waiter);
            if (waiter->cont.msg_buf != NULL) {
                /* signal 方在锁内完成移交(4 字节);失败不回滚——字已被
                 * 消费,等待者以 FAULT 醒来比静默丢字更可诊断。 */
                if (copy_to_user(waiter->cont.msg_buf, &consumed,
                                 sizeof(consumed)) != KERN_OK) {
                    waiter->cont.result = KERN_ERR_FAULT;
                    syscall_cont_wake(waiter, KERN_ERR_FAULT);
                    irq_spin_unlock(&notification_lock, crit);
                    return KERN_OK;
                }
            }
            waiter->cont.object = NULL;
            waiter->cont.result = KERN_OK;
            syscall_cont_wake(waiter, KERN_OK);
        }
    }

    irq_spin_unlock(&notification_lock, crit);
    return KERN_OK;
}

kern_err_t notification_signal(notification_id_t id, uint32_t badge) {
    uint32_t crit = irq_spin_lock(&notification_lock);
    notification_t *ntfn = get_notification(id);
    if (ntfn == NULL) {
        irq_spin_unlock(&notification_lock, crit);
        return KERN_ERR_PARAM;
    }
    return ntfn_signal_locked(ntfn, badge, crit);
}

kern_err_t notification_signal_obj(void *obj, uint32_t badge) {
    notification_t *ntfn = (notification_t *)obj;
    if (ntfn == NULL || !ntfn->in_use) {
        return KERN_ERR_PARAM;
    }
    uint32_t crit = irq_spin_lock(&notification_lock);
    if (!ntfn->in_use) {
        irq_spin_unlock(&notification_lock, crit);
        return KERN_ERR_PARAM;
    }
    return ntfn_signal_locked(ntfn, badge, crit);
}

/*============================================================================
 * poll: 消费型非阻塞取整字
 *============================================================================*/

kern_err_t notification_poll(notification_id_t id, uint32_t *out_word) {
    uint32_t crit = irq_spin_lock(&notification_lock);

    notification_t *ntfn = get_notification(id);
    if (ntfn == NULL) {
        irq_spin_unlock(&notification_lock, crit);
        return KERN_ERR_PARAM;
    }

    uint32_t w = ntfn->word;
    ntfn->word = 0;
    if (out_word != NULL) {
        *out_word = w;
    }

    irq_spin_unlock(&notification_lock, crit);
    return KERN_OK;
}

/*============================================================================
 * 阻塞 wait (两阶段 continuation,仅 SVC 上下文)
 *============================================================================*/

#if SYSCALL_ENABLE
kern_err_t notification_wait_syscall(notification_id_t id, uint32_t timeout,
                                     void *user_word_out) {
    if (hal_irq_get_active() >= 0) {
        return KERN_ERR_ISR;
    }
    if (user_word_out != NULL &&
        !user_access_ok(user_word_out, sizeof(uint32_t), USER_ACCESS_WRITE)) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = irq_spin_lock(&notification_lock);

    notification_t *ntfn = get_notification(id);
    if (ntfn == NULL) {
        irq_spin_unlock(&notification_lock, crit);
        return KERN_ERR_PARAM;
    }

    /* fast path: word 非零 → 取清整字立即返回 */
    if (ntfn->word != 0U) {
        uint32_t w = ntfn->word;
        ntfn->word = 0;
        irq_spin_unlock(&notification_lock, crit);
        if (user_word_out != NULL) {
            if (copy_to_user(user_word_out, &w, sizeof(w)) != KERN_OK) {
                return KERN_ERR_FAULT;
            }
        }
        return KERN_OK;
    }

    tcb_t *current = sched_get_current();
    if (current == NULL ||
        current->id < 0 || current->id >= KERN_MAX_TASKS) {
        irq_spin_unlock(&notification_lock, crit);
        return KERN_ERR_STATE;
    }

    current->cont.msg_buf = user_word_out;

    syscall_cont_prepare_locked(BLOCK_REASON_NOTIFICATION, ntfn);
    wait_queue_add(&ntfn->wait_queue, current);

    irq_spin_unlock(&notification_lock, crit);
    return syscall_cont_commit(timeout);
}
#endif /* SYSCALL_ENABLE */

#endif /* IPC_NOTIFICATION */
