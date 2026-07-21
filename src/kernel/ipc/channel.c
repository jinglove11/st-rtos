/**
 * @file channel.c
 * @brief Channel (P2P) 双向通信实现
 *
 * 一对一模型：两个任务通过 channel 双向通信，
 * 附带共享内存区域。
 */

#include "channel.h"
#include "wait_queue.h"
#include "scheduler.h"
#include "task.h"
#include "kernel_config.h"
#include "kernel_types.h"
#include "hal.h"
#include "spinlock.h"
#include "trace.h"
#include "syscall.h"
#include "capability.h"
#include <string.h>

#if IPC_CHANNEL

/*============================================================================
 * 静态分配
 *============================================================================*/

typedef struct {
    kobject_header_t hdr;            // M2-Step3b: 对象 header
    task_id_t   peer_a;
    task_id_t   peer_b;
    void       *shm;
    uint32_t    shm_size;

    /* A→B 方向 */
    uint8_t    *a_to_b_buf;
    uint8_t     a_to_b_ready;       // 1=有数据

    /* B→A 方向 */
    uint8_t    *b_to_a_buf;
    uint8_t     b_to_a_ready;       // 1=有数据

    wait_queue_t a_recv_waiters;    // A 等待 B 发送
    wait_queue_t b_recv_waiters;    // B 等待 A 发送
    wait_queue_t a_send_waiters;    // A 等待 A→B 槽位
    wait_queue_t b_send_waiters;    // B 等待 B→A 槽位

    uint16_t    msg_size;
    uint8_t     in_use;
} channel_t;

static channel_t ch_pool[KERN_MAX_CHANNELS];
static uint32_t ch_used_bitmap;
static irq_spinlock_t ch_lock; /* M1: SMP safe */

/* 消消息缓冲区: [channel][direction 0=a_to_b, 1=b_to_a][msg_size] */
static uint8_t ch_msg_buffers[KERN_MAX_CHANNELS][2][KERN_CH_MSG_SIZE]
    __attribute__((aligned(4)));

static cap_id_t ch_cap_id_buffers[KERN_MAX_CHANNELS][2][IPC_CAPS_MAX];
static uint8_t ch_cap_count_buffers[KERN_MAX_CHANNELS][2];

/* 共享内存池 */
static uint8_t ch_shm_pool[KERN_MAX_CHANNELS][256]
    __attribute__((aligned(8)));

#if SYSCALL_ENABLE
static uint8_t ch_syscall_send_msg[KERNEL_MAX_TASKS][KERN_CH_MSG_SIZE]
    __attribute__((aligned(4)));
static ipc_cap_xfer_t ch_syscall_send_caps[KERNEL_MAX_TASKS][IPC_CAPS_MAX];
static uint8_t ch_syscall_send_cap_count[KERNEL_MAX_TASKS];
static void *ch_syscall_recv_msg[KERNEL_MAX_TASKS];
static cap_id_t *ch_syscall_recv_caps[KERNEL_MAX_TASKS];
static uint8_t *ch_syscall_recv_cap_count[KERNEL_MAX_TASKS];
#endif

/*============================================================================
 * 内部函数
 *============================================================================*/

static ch_id_t alloc_ch_id(void) {
    for (int i = 0; i < KERN_MAX_CHANNELS; i++) {
        if (!(ch_used_bitmap & (1U << i))) {
            ch_used_bitmap |= (1U << i);
            return (ch_id_t)i;
        }
    }
    return KERN_INVALID_ID;
}

static void free_ch_id(ch_id_t id) {
    if (id >= 0 && id < KERN_MAX_CHANNELS) {
        ch_used_bitmap &= ~(1U << id);
    }
}

static channel_t *ch_get(ch_id_t id) {
    if (id < 0 || id >= KERN_MAX_CHANNELS) return NULL;
    if (!ch_pool[id].in_use) return NULL;
    return &ch_pool[id];
}

static void channel_wake_all(wait_queue_t *queue, kern_err_t result) {
    tcb_t *tcb = queue->head;

    while (tcb) {
        tcb_t *next = tcb->wait_next;
        tcb->wait_next = NULL;
        tcb->wait_prev = NULL;
#if SYSCALL_ENABLE
        if (tcb->id >= 0 && tcb->id < KERNEL_MAX_TASKS) {
            memset(ch_syscall_send_msg[tcb->id], 0,
                   sizeof(ch_syscall_send_msg[tcb->id]));
            memset(ch_syscall_send_caps[tcb->id], 0,
                   sizeof(ch_syscall_send_caps[tcb->id]));
            ch_syscall_send_cap_count[tcb->id] = 0;
            ch_syscall_recv_msg[tcb->id] = NULL;
            ch_syscall_recv_caps[tcb->id] = NULL;
            ch_syscall_recv_cap_count[tcb->id] = NULL;
        }
#endif
        tcb->block_result = result;
        sched_wakeup(tcb, result);
        tcb = next;
    }

    wait_queue_init(queue);
}

static kern_err_t channel_get_side(channel_t *ch, tcb_t *current, int *is_a) {
    if (ch == NULL || current == NULL) {
        return KERN_ERR_STATE;
    }
    if (ch->peer_a == KERN_INVALID_ID || ch->peer_b == KERN_INVALID_ID) {
        return KERN_ERR_STATE;
    }
    if (current->id == ch->peer_a) {
        if (task_get_tcb(ch->peer_b) == NULL) {
            return KERN_ERR_NOEXIST;
        }
        *is_a = 1;
        return KERN_OK;
    }
    if (current->id == ch->peer_b) {
        if (task_get_tcb(ch->peer_a) == NULL) {
            return KERN_ERR_NOEXIST;
        }
        *is_a = 0;
        return KERN_OK;
    }
    return KERN_ERR_PERM;
}

static kern_err_t channel_get_recv_side(channel_t *ch, tcb_t *current,
                                        int *is_a) {
    if (ch == NULL || current == NULL) {
        return KERN_ERR_STATE;
    }
    if (ch->peer_a == KERN_INVALID_ID && ch->peer_b == KERN_INVALID_ID) {
        return KERN_ERR_STATE;
    }
    if (current->id == ch->peer_a) {
        *is_a = 1;
        return KERN_OK;
    }
    if (current->id == ch->peer_b) {
        *is_a = 0;
        return KERN_OK;
    }
    return KERN_ERR_PERM;
}

static int channel_sender_alive(channel_t *ch, int receiver_is_a) {
    task_id_t sender = receiver_is_a ? ch->peer_b : ch->peer_a;
    return sender != KERN_INVALID_ID && task_get_tcb(sender) != NULL;
}

static void channel_wake_recv_waiter(wait_queue_t *recv_wq,
                                     uint8_t *buf,
                                     uint8_t *ready_flag,
                                     cap_id_t *cap_src,
                                     uint8_t *cap_count_src) {
    if (recv_wq->count == 0 || !*ready_flag) {
        return;
    }

    tcb_t *waiter = wait_queue_get_highest(recv_wq);
    if (waiter == NULL) {
        return;
    }

#if SYSCALL_ENABLE
    if (waiter->syscall_blocked &&
        waiter->id >= 0 && waiter->id < KERNEL_MAX_TASKS &&
        ch_syscall_recv_msg[waiter->id] != NULL) {
        if (*cap_count_src > 0) {
            if (ch_syscall_recv_caps[waiter->id] == NULL ||
                ch_syscall_recv_cap_count[waiter->id] == NULL) {
                ch_syscall_recv_msg[waiter->id] = NULL;
                wait_queue_remove(recv_wq, waiter);
                waiter->block_result = KERN_ERR_RESOURCE;
                sched_wakeup(waiter, KERN_ERR_RESOURCE);
                return;
            }
            memcpy(ch_syscall_recv_caps[waiter->id], cap_src,
                   sizeof(cap_id_t) * (*cap_count_src));
            *ch_syscall_recv_cap_count[waiter->id] = *cap_count_src;
        } else if (ch_syscall_recv_cap_count[waiter->id] != NULL) {
            *ch_syscall_recv_cap_count[waiter->id] = 0;
        }

        memcpy(ch_syscall_recv_msg[waiter->id], buf, KERN_CH_MSG_SIZE);
        ch_syscall_recv_msg[waiter->id] = NULL;
        ch_syscall_recv_caps[waiter->id] = NULL;
        ch_syscall_recv_cap_count[waiter->id] = NULL;
        *ready_flag = 0;
        *cap_count_src = 0;
        memset(cap_src, 0, sizeof(cap_id_t) * IPC_CAPS_MAX);
        wait_queue_remove(recv_wq, waiter);
        waiter->block_result = KERN_OK;
        sched_wakeup(waiter, KERN_OK);
        return;
    }
#endif

    wait_queue_remove(recv_wq, waiter);
    waiter->block_result = KERN_OK;
    sched_wakeup(waiter, KERN_OK);
}

static void channel_wake_send_waiter(wait_queue_t *send_wq,
                                     wait_queue_t *recv_wq,
                                     uint8_t *buf,
                                     uint8_t *ready_flag,
                                     cap_id_t *cap_dst,
                                     uint8_t *cap_count_dst,
                                     task_id_t receiver_id) {
    if (send_wq->count == 0 || *ready_flag) {
        return;
    }

    tcb_t *waiter = wait_queue_get_highest(send_wq);
    if (waiter == NULL) {
        return;
    }

#if SYSCALL_ENABLE
    if (waiter->syscall_blocked &&
        waiter->id >= 0 && waiter->id < KERNEL_MAX_TASKS) {
        uint8_t cap_count = ch_syscall_send_cap_count[waiter->id];
        if (cap_count > 0) {
            tcb_t *receiver = task_get_tcb(receiver_id);
            kern_err_t xfer_err = ipc_transfer_caps(waiter, receiver,
                                                    ch_syscall_send_caps[waiter->id],
                                                    cap_count, cap_dst);
            if (xfer_err != KERN_OK) {
                memset(ch_syscall_send_msg[waiter->id], 0,
                       sizeof(ch_syscall_send_msg[waiter->id]));
                memset(ch_syscall_send_caps[waiter->id], 0,
                       sizeof(ch_syscall_send_caps[waiter->id]));
                ch_syscall_send_cap_count[waiter->id] = 0;
                wait_queue_remove(send_wq, waiter);
                waiter->block_result = xfer_err;
                sched_wakeup(waiter, xfer_err);
                return;
            }
        } else {
            memset(cap_dst, 0, sizeof(cap_id_t) * IPC_CAPS_MAX);
        }

        memcpy(buf, ch_syscall_send_msg[waiter->id], KERN_CH_MSG_SIZE);
        memset(ch_syscall_send_msg[waiter->id], 0,
               sizeof(ch_syscall_send_msg[waiter->id]));
        memset(ch_syscall_send_caps[waiter->id], 0,
               sizeof(ch_syscall_send_caps[waiter->id]));
        ch_syscall_send_cap_count[waiter->id] = 0;
        *cap_count_dst = cap_count;
        *ready_flag = 1;
        wait_queue_remove(send_wq, waiter);
        waiter->block_result = KERN_OK;
        sched_wakeup(waiter, KERN_OK);
        channel_wake_recv_waiter(recv_wq, buf, ready_flag, cap_dst,
                                 cap_count_dst);
        return;
    }
#endif

    wait_queue_remove(send_wq, waiter);
    waiter->block_result = KERN_OK;
    sched_wakeup(waiter, KERN_OK);
}

/*============================================================================
 * 公开接口
 *============================================================================*/

void channel_init(void) {
    irq_spin_init(&ch_lock);
    memset(ch_pool, 0, sizeof(ch_pool));
    memset(ch_cap_id_buffers, 0, sizeof(ch_cap_id_buffers));
    memset(ch_cap_count_buffers, 0, sizeof(ch_cap_count_buffers));
#if SYSCALL_ENABLE
    memset(ch_syscall_send_msg, 0, sizeof(ch_syscall_send_msg));
    memset(ch_syscall_send_caps, 0, sizeof(ch_syscall_send_caps));
    memset(ch_syscall_send_cap_count, 0, sizeof(ch_syscall_send_cap_count));
    memset(ch_syscall_recv_msg, 0, sizeof(ch_syscall_recv_msg));
    memset(ch_syscall_recv_caps, 0, sizeof(ch_syscall_recv_caps));
    memset(ch_syscall_recv_cap_count, 0, sizeof(ch_syscall_recv_cap_count));
#endif
    ch_used_bitmap = 0;
}

ch_id_t channel_create(uint16_t msg_size, uint32_t shm_size) {
    if (msg_size == 0 || msg_size > KERN_CH_MSG_SIZE) return KERN_INVALID_ID;

    uint32_t crit = irq_spin_lock(&ch_lock);

    ch_id_t id = alloc_ch_id();
    if (id == KERN_INVALID_ID) {
        irq_spin_unlock(&ch_lock, crit);
        return KERN_INVALID_ID;
    }

    channel_t *ch = &ch_pool[id];
    /* M2-Step3b: 跨 memset 保留 generation */
    uint16_t saved_gen = ch->hdr.generation;
    memset(ch, 0, sizeof(channel_t));
    kobj_header_init(&ch->hdr, CAP_OBJ_CHANNEL);
    if (saved_gen != 0) {
        ch->hdr.generation = saved_gen;
    }

    ch->peer_a   = KERN_INVALID_ID;
    ch->peer_b   = KERN_INVALID_ID;
    ch->msg_size = msg_size;

    ch->a_to_b_buf = ch_msg_buffers[id][0];
    ch->b_to_a_buf = ch_msg_buffers[id][1];
    ch->a_to_b_ready = 0;
    ch->b_to_a_ready = 0;

    /* 共享内存 */
    if (shm_size > 0) {
        if (shm_size > sizeof(ch_shm_pool[id])) {
            shm_size = sizeof(ch_shm_pool[id]);
        }
        ch->shm = ch_shm_pool[id];
        ch->shm_size = shm_size;
        memset(ch->shm, 0, shm_size);
    } else {
        ch->shm = NULL;
        ch->shm_size = 0;
    }

    wait_queue_init(&ch->a_recv_waiters);
    wait_queue_init(&ch->b_recv_waiters);
    wait_queue_init(&ch->a_send_waiters);
    wait_queue_init(&ch->b_send_waiters);

    ch->in_use = 1;

    irq_spin_unlock(&ch_lock, crit);
    return id;
}

kern_err_t channel_delete(ch_id_t ch_id) {
    uint32_t crit = irq_spin_lock(&ch_lock);

    channel_t *ch = ch_get(ch_id);
    if (ch == NULL) {
        irq_spin_unlock(&ch_lock, crit);
        return KERN_ERR_PARAM;
    }

    /* 唤醒所有等待的任务 */
    channel_wake_all(&ch->a_recv_waiters, KERN_ERR_NOEXIST);
    channel_wake_all(&ch->b_recv_waiters, KERN_ERR_NOEXIST);
    channel_wake_all(&ch->a_send_waiters, KERN_ERR_NOEXIST);
    channel_wake_all(&ch->b_send_waiters, KERN_ERR_NOEXIST);
    memset(ch_cap_id_buffers[ch_id], 0, sizeof(ch_cap_id_buffers[ch_id]));
    memset(ch_cap_count_buffers[ch_id], 0, sizeof(ch_cap_count_buffers[ch_id]));

#if CAP_ENABLE
    /* M2-Step1+3b: 撤销所有任务持有的指向此 channel 的 cap。Step3b 改真指针。 */
    (void)cap_revoke_object(ch, CAP_OBJ_CHANNEL);
#endif
    /* M2-Step3b: bump generation 跨 memset 保留 */
    uint16_t next_gen = kobj_header_prepare_reuse(&ch->hdr);
    memset(ch, 0, sizeof(channel_t));
    ch->hdr.obj_type   = CAP_OBJ_CHANNEL;
    ch->hdr.generation = next_gen;
    free_ch_id(ch_id);

    irq_spin_unlock(&ch_lock, crit);
    return KERN_OK;
}

/* M2-Step3b: cap 路径 id ↔ 对象指针 转换。 */
ch_id_t channel_id_from_obj(void *obj) {
    if (obj == NULL) return KERN_INVALID_ID;
    channel_t *ch = (channel_t *)obj;
    ch_id_t id = (ch_id_t)(ch - ch_pool);
    if (id < 0 || id >= KERN_MAX_CHANNELS) return KERN_INVALID_ID;
    return id;
}

void *channel_obj_for_cap(ch_id_t id) {
    if (id < 0 || id >= KERN_MAX_CHANNELS) return NULL;
    return (void *)&ch_pool[id];
}

void channel_cleanup_task(void *channel_obj, tcb_t *tcb) {
    channel_t *ch = (channel_t *)channel_obj;

    if (ch == NULL || tcb == NULL) {
        return;
    }

    wait_queue_remove_safe(&ch->a_recv_waiters, tcb);
    wait_queue_remove_safe(&ch->b_recv_waiters, tcb);
    wait_queue_remove_safe(&ch->a_send_waiters, tcb);
    wait_queue_remove_safe(&ch->b_send_waiters, tcb);

#if SYSCALL_ENABLE
    if (tcb->id >= 0 && tcb->id < KERNEL_MAX_TASKS) {
        memset(ch_syscall_send_msg[tcb->id], 0,
               sizeof(ch_syscall_send_msg[tcb->id]));
        memset(ch_syscall_send_caps[tcb->id], 0,
               sizeof(ch_syscall_send_caps[tcb->id]));
        ch_syscall_send_cap_count[tcb->id] = 0;
        ch_syscall_recv_msg[tcb->id] = NULL;
        ch_syscall_recv_caps[tcb->id] = NULL;
        ch_syscall_recv_cap_count[tcb->id] = NULL;
    }
#endif

    if (tcb->id == ch->peer_a) {
        ch->peer_a = KERN_INVALID_ID;
    }
    if (tcb->id == ch->peer_b) {
        ch->peer_b = KERN_INVALID_ID;
    }
}

kern_err_t channel_connect(ch_id_t ch_id, task_id_t peer_a, task_id_t peer_b) {
    uint32_t crit = irq_spin_lock(&ch_lock);

    channel_t *ch = ch_get(ch_id);
    if (ch == NULL) {
        irq_spin_unlock(&ch_lock, crit);
        return KERN_ERR_PARAM;
    }

    ch->peer_a = peer_a;
    ch->peer_b = peer_b;

    irq_spin_unlock(&ch_lock, crit);
    return KERN_OK;
}

static kern_err_t channel_send_common(ch_id_t ch_id,
                                      const void *msg,
                                      const ipc_cap_xfer_t *caps,
                                      uint8_t cap_count,
                                      uint32_t timeout) {
    if (hal_irq_get_active() >= 0) return KERN_ERR_ISR;
    if (cap_count > IPC_CAPS_MAX) return KERN_ERR_PARAM;
    if (cap_count > 0 && caps == NULL) return KERN_ERR_PARAM;

    uint32_t crit = irq_spin_lock(&ch_lock);

    channel_t *ch = ch_get(ch_id);
    if (ch == NULL) {
        irq_spin_unlock(&ch_lock, crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();
    trace_record(TRACE_IPC_SEND, (uint8_t)current->id, (uint16_t)ch_id);
    int is_a = 0;
    kern_err_t side_err = channel_get_side(ch, current, &is_a);
    if (side_err != KERN_OK) {
        irq_spin_unlock(&ch_lock, crit);
        return side_err;
    }

    /* 确定方向和目标缓冲区 */
    uint8_t *dst_buf;
    uint8_t *ready_flag;
    wait_queue_t *recv_wq;
    wait_queue_t *send_wq;
    cap_id_t *cap_dst;
    uint8_t *cap_count_dst;
    task_id_t receiver_id;

    if (is_a) {
        dst_buf  = ch->a_to_b_buf;
        ready_flag = &ch->a_to_b_ready;
        recv_wq  = &ch->b_recv_waiters;
        send_wq = &ch->a_send_waiters;
        cap_dst = ch_cap_id_buffers[ch_id][0];
        cap_count_dst = &ch_cap_count_buffers[ch_id][0];
        receiver_id = ch->peer_b;
    } else {
        dst_buf  = ch->b_to_a_buf;
        ready_flag = &ch->b_to_a_ready;
        recv_wq  = &ch->a_recv_waiters;
        send_wq = &ch->b_send_waiters;
        cap_dst = ch_cap_id_buffers[ch_id][1];
        cap_count_dst = &ch_cap_count_buffers[ch_id][1];
        receiver_id = ch->peer_a;
    }

    /* 如果对端还没读取上一条消息，等待 */
    if (*ready_flag) {
        if (timeout == 0) {
            irq_spin_unlock(&ch_lock, crit);
            return KERN_ERR_TIMEOUT;
        }

        wait_queue_add(send_wq, current);

        {
            extern void sched_remove_ready(tcb_t *tcb);
            sched_remove_ready(current);
        }
        current->state = TASK_STATE_BLOCKED;
        current->block_reason = BLOCK_REASON_CH_SEND;
        current->block_obj = ch;
        current->block_result = KERN_OK;
        if (timeout > 0) {
            extern uint32_t sched_get_tick_count(void);
            current->wake_tick = sched_get_tick_count() + timeout;
        } else {
            current->wake_tick = 0;
        }

        irq_spin_unlock(&ch_lock, crit);
        hal_trigger_pendsv();

        while (current->state == TASK_STATE_BLOCKED) {
            __asm volatile("wfi");
            __asm volatile("dmb");
        }

        if (current->block_result != KERN_OK) {
            crit = irq_spin_lock(&ch_lock);
            if (current->block_obj == ch) {
                wait_queue_remove_safe(send_wq, current);
                current->block_obj = NULL;
            }
            irq_spin_unlock(&ch_lock, crit);
            return current->block_result;
        }

        crit = irq_spin_lock(&ch_lock);
        ch = ch_get(ch_id);
        if (ch == NULL) {
            irq_spin_unlock(&ch_lock, crit);
            return KERN_ERR_NOEXIST;
        }

        side_err = channel_get_side(ch, current, &is_a);
        if (side_err != KERN_OK) {
            irq_spin_unlock(&ch_lock, crit);
            return side_err;
        }
        if (is_a) {
            dst_buf  = ch->a_to_b_buf;
            ready_flag = &ch->a_to_b_ready;
            recv_wq  = &ch->b_recv_waiters;
            cap_dst = ch_cap_id_buffers[ch_id][0];
            cap_count_dst = &ch_cap_count_buffers[ch_id][0];
            receiver_id = ch->peer_b;
        } else {
            dst_buf  = ch->b_to_a_buf;
            ready_flag = &ch->b_to_a_ready;
            recv_wq  = &ch->a_recv_waiters;
            cap_dst = ch_cap_id_buffers[ch_id][1];
            cap_count_dst = &ch_cap_count_buffers[ch_id][1];
            receiver_id = ch->peer_a;
        }
        if (*ready_flag) {
            irq_spin_unlock(&ch_lock, crit);
            return KERN_ERR_BUSY;
        }
    }

    if (cap_count > 0) {
        tcb_t *receiver = task_get_tcb(receiver_id);
        kern_err_t xfer_err = ipc_transfer_caps(current, receiver,
                                                caps, cap_count, cap_dst);
        if (xfer_err != KERN_OK) {
            irq_spin_unlock(&ch_lock, crit);
            return xfer_err;
        }
    }

    /* 写入消息 */
    memcpy(dst_buf, msg, ch->msg_size);
    *cap_count_dst = cap_count;
    *ready_flag = 1;

    /* 如果对端在等待接收，唤醒它 */
    channel_wake_recv_waiter(recv_wq, dst_buf, ready_flag, cap_dst,
                             cap_count_dst);

    irq_spin_unlock(&ch_lock, crit);
    return KERN_OK;
}

kern_err_t channel_send(ch_id_t ch_id, const void *msg, uint32_t timeout) {
    return channel_send_common(ch_id, msg, NULL, 0, timeout);
}

#if SYSCALL_ENABLE
kern_err_t channel_send_syscall(ch_id_t ch_id, const void *msg,
                                uint32_t timeout) {
    if (hal_irq_get_active() >= 0) return KERN_ERR_ISR;

    uint32_t crit = irq_spin_lock(&ch_lock);

    channel_t *ch = ch_get(ch_id);
    if (ch == NULL || msg == NULL) {
        irq_spin_unlock(&ch_lock, crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();
    if (current == NULL ||
        current->id < 0 || current->id >= KERNEL_MAX_TASKS) {
        irq_spin_unlock(&ch_lock, crit);
        return KERN_ERR_STATE;
    }
    trace_record(TRACE_IPC_SEND, (uint8_t)current->id, (uint16_t)ch_id);

    int is_a = 0;
    kern_err_t side_err = channel_get_side(ch, current, &is_a);
    if (side_err != KERN_OK) {
        irq_spin_unlock(&ch_lock, crit);
        return side_err;
    }

    uint8_t *dst_buf;
    uint8_t *ready_flag;
    wait_queue_t *recv_wq;
    wait_queue_t *send_wq;
    cap_id_t *cap_dst;
    uint8_t *cap_count_dst;

    if (is_a) {
        dst_buf = ch->a_to_b_buf;
        ready_flag = &ch->a_to_b_ready;
        recv_wq = &ch->b_recv_waiters;
        send_wq = &ch->a_send_waiters;
        cap_dst = ch_cap_id_buffers[ch_id][0];
        cap_count_dst = &ch_cap_count_buffers[ch_id][0];
    } else {
        dst_buf = ch->b_to_a_buf;
        ready_flag = &ch->b_to_a_ready;
        recv_wq = &ch->a_recv_waiters;
        send_wq = &ch->b_send_waiters;
        cap_dst = ch_cap_id_buffers[ch_id][1];
        cap_count_dst = &ch_cap_count_buffers[ch_id][1];
    }

    if (*ready_flag) {
        if (timeout == 0) {
            irq_spin_unlock(&ch_lock, crit);
            return KERN_ERR_TIMEOUT;
        }

        memcpy(ch_syscall_send_msg[current->id], msg, ch->msg_size);
        current->syscall_blocked = 1;
        current->block_reason = BLOCK_REASON_CH_SEND;
        current->block_obj = ch;
        current->block_result = KERN_OK;
        wait_queue_add(send_wq, current);
        {
            extern void sched_remove_ready(tcb_t *tcb);
            sched_remove_ready(current);
        }
        current->state = TASK_STATE_BLOCKED;
        if (timeout > 0) {
            extern uint32_t sched_get_tick_count(void);
            current->wake_tick = sched_get_tick_count() + timeout;
        } else {
            current->wake_tick = 0;
        }

        irq_spin_unlock(&ch_lock, crit);
        return KERN_SYSCALL_BLOCKED;
    }

    memcpy(dst_buf, msg, ch->msg_size);
    *cap_count_dst = 0;
    memset(cap_dst, 0, sizeof(cap_id_t) * IPC_CAPS_MAX);
    *ready_flag = 1;

    channel_wake_recv_waiter(recv_wq, dst_buf, ready_flag, cap_dst,
                             cap_count_dst);

    irq_spin_unlock(&ch_lock, crit);
    return KERN_OK;
}

kern_err_t channel_send_caps_syscall(ch_id_t ch_id,
                                     const void *msg,
                                     const ipc_cap_xfer_t *caps,
                                     uint8_t cap_count,
                                     uint32_t timeout) {
    if (hal_irq_get_active() >= 0) return KERN_ERR_ISR;
    if (cap_count > IPC_CAPS_MAX) return KERN_ERR_PARAM;
    if (cap_count > 0 && caps == NULL) return KERN_ERR_PARAM;

    uint32_t crit = irq_spin_lock(&ch_lock);

    channel_t *ch = ch_get(ch_id);
    if (ch == NULL || msg == NULL) {
        irq_spin_unlock(&ch_lock, crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();
    if (current == NULL ||
        current->id < 0 || current->id >= KERNEL_MAX_TASKS) {
        irq_spin_unlock(&ch_lock, crit);
        return KERN_ERR_STATE;
    }
    trace_record(TRACE_IPC_SEND, (uint8_t)current->id, (uint16_t)ch_id);

    int is_a = 0;
    kern_err_t side_err = channel_get_side(ch, current, &is_a);
    if (side_err != KERN_OK) {
        irq_spin_unlock(&ch_lock, crit);
        return side_err;
    }

    uint8_t *dst_buf;
    uint8_t *ready_flag;
    wait_queue_t *recv_wq;
    wait_queue_t *send_wq;
    cap_id_t *cap_dst;
    uint8_t *cap_count_dst;
    task_id_t receiver_id;

    if (is_a) {
        dst_buf = ch->a_to_b_buf;
        ready_flag = &ch->a_to_b_ready;
        recv_wq = &ch->b_recv_waiters;
        send_wq = &ch->a_send_waiters;
        cap_dst = ch_cap_id_buffers[ch_id][0];
        cap_count_dst = &ch_cap_count_buffers[ch_id][0];
        receiver_id = ch->peer_b;
    } else {
        dst_buf = ch->b_to_a_buf;
        ready_flag = &ch->b_to_a_ready;
        recv_wq = &ch->a_recv_waiters;
        send_wq = &ch->b_send_waiters;
        cap_dst = ch_cap_id_buffers[ch_id][1];
        cap_count_dst = &ch_cap_count_buffers[ch_id][1];
        receiver_id = ch->peer_a;
    }

    if (*ready_flag) {
        if (timeout == 0) {
            irq_spin_unlock(&ch_lock, crit);
            return KERN_ERR_TIMEOUT;
        }

        memcpy(ch_syscall_send_msg[current->id], msg, ch->msg_size);
        if (cap_count > 0) {
            memcpy(ch_syscall_send_caps[current->id], caps,
                   sizeof(ipc_cap_xfer_t) * cap_count);
        }
        ch_syscall_send_cap_count[current->id] = cap_count;
        current->syscall_blocked = 1;
        current->block_reason = BLOCK_REASON_CH_SEND;
        current->block_obj = ch;
        current->block_result = KERN_OK;
        wait_queue_add(send_wq, current);
        {
            extern void sched_remove_ready(tcb_t *tcb);
            sched_remove_ready(current);
        }
        current->state = TASK_STATE_BLOCKED;
        if (timeout > 0) {
            extern uint32_t sched_get_tick_count(void);
            current->wake_tick = sched_get_tick_count() + timeout;
        } else {
            current->wake_tick = 0;
        }

        irq_spin_unlock(&ch_lock, crit);
        return KERN_SYSCALL_BLOCKED;
    }

    if (cap_count > 0) {
        tcb_t *receiver = task_get_tcb(receiver_id);
        kern_err_t xfer_err = ipc_transfer_caps(current, receiver,
                                                caps, cap_count, cap_dst);
        if (xfer_err != KERN_OK) {
            irq_spin_unlock(&ch_lock, crit);
            return xfer_err;
        }
    } else {
        memset(cap_dst, 0, sizeof(cap_id_t) * IPC_CAPS_MAX);
    }

    memcpy(dst_buf, msg, ch->msg_size);
    *cap_count_dst = cap_count;
    *ready_flag = 1;

    channel_wake_recv_waiter(recv_wq, dst_buf, ready_flag, cap_dst,
                             cap_count_dst);

    irq_spin_unlock(&ch_lock, crit);
    return KERN_OK;
}
#endif

kern_err_t channel_send_caps(ch_id_t ch_id,
                             const void *msg,
                             const ipc_cap_xfer_t *caps,
                             uint8_t cap_count,
                             uint32_t timeout) {
    return channel_send_common(ch_id, msg, caps, cap_count, timeout);
}

static kern_err_t channel_recv_common(ch_id_t ch_id,
                                      void *msg,
                                      cap_id_t *out_caps,
                                      uint8_t *out_cap_count,
                                      uint32_t timeout) {
    if (hal_irq_get_active() >= 0) return KERN_ERR_ISR;

    uint32_t crit = irq_spin_lock(&ch_lock);

    channel_t *ch = ch_get(ch_id);
    if (ch == NULL) {
        irq_spin_unlock(&ch_lock, crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();
    trace_record(TRACE_IPC_RECV, (uint8_t)current->id, (uint16_t)ch_id);
    int is_a = 0;
    kern_err_t side_err = channel_get_recv_side(ch, current, &is_a);
    if (side_err != KERN_OK) {
        irq_spin_unlock(&ch_lock, crit);
        return side_err;
    }

    /* 确定源缓冲区 */
    uint8_t *src_buf;
    uint8_t *ready_flag;
    wait_queue_t *recv_wq;
    wait_queue_t *send_wq;
    cap_id_t *cap_src;
    uint8_t *cap_count_src;

    if (is_a) {
        /* A 接收 B 发来的消息 (b_to_a) */
        src_buf   = ch->b_to_a_buf;
        ready_flag = &ch->b_to_a_ready;
        recv_wq   = &ch->a_recv_waiters;
        send_wq   = &ch->b_send_waiters;
        cap_src = ch_cap_id_buffers[ch_id][1];
        cap_count_src = &ch_cap_count_buffers[ch_id][1];
    } else {
        /* B 接收 A 发来的消息 (a_to_b) */
        src_buf   = ch->a_to_b_buf;
        ready_flag = &ch->a_to_b_ready;
        recv_wq   = &ch->b_recv_waiters;
        send_wq   = &ch->a_send_waiters;
        cap_src = ch_cap_id_buffers[ch_id][0];
        cap_count_src = &ch_cap_count_buffers[ch_id][0];
    }

    /* 如果没有数据，阻塞等待 */
    if (!*ready_flag) {
        if (!channel_sender_alive(ch, is_a)) {
            irq_spin_unlock(&ch_lock, crit);
            return KERN_ERR_NOEXIST;
        }
        if (timeout == 0) {
            irq_spin_unlock(&ch_lock, crit);
            return KERN_ERR_TIMEOUT;
        }

        wait_queue_add(recv_wq, current);

        {
            extern void sched_remove_ready(tcb_t *tcb);
            sched_remove_ready(current);
        }
        current->state = TASK_STATE_BLOCKED;
        current->block_reason = is_a ? BLOCK_REASON_CH_RECV : BLOCK_REASON_CH_RECV;
        current->block_obj = ch;
        current->block_result = KERN_OK;
        if (timeout > 0) {
            extern uint32_t sched_get_tick_count(void);
            current->wake_tick = sched_get_tick_count() + timeout;
        } else {
            current->wake_tick = 0;
        }

        irq_spin_unlock(&ch_lock, crit);
        hal_trigger_pendsv();

        while (current->state == TASK_STATE_BLOCKED) {
            __asm volatile("wfi");
            __asm volatile("dmb");
        }

        if (current->block_result != KERN_OK) {
            crit = irq_spin_lock(&ch_lock);
            if (current->block_obj == ch) {
                wait_queue_remove_safe(recv_wq, current);
                current->block_obj = NULL;
            }
            irq_spin_unlock(&ch_lock, crit);
            return current->block_result;
        }

        crit = irq_spin_lock(&ch_lock);
        ch = ch_get(ch_id);
        if (ch == NULL) {
            irq_spin_unlock(&ch_lock, crit);
            return KERN_ERR_NOEXIST;
        }

        side_err = channel_get_recv_side(ch, current, &is_a);
        if (side_err != KERN_OK) {
            irq_spin_unlock(&ch_lock, crit);
            return side_err;
        }
        if (is_a) {
            src_buf   = ch->b_to_a_buf;
            ready_flag = &ch->b_to_a_ready;
            send_wq   = &ch->b_send_waiters;
            cap_src = ch_cap_id_buffers[ch_id][1];
            cap_count_src = &ch_cap_count_buffers[ch_id][1];
        } else {
            src_buf   = ch->a_to_b_buf;
            ready_flag = &ch->a_to_b_ready;
            send_wq   = &ch->a_send_waiters;
            cap_src = ch_cap_id_buffers[ch_id][0];
            cap_count_src = &ch_cap_count_buffers[ch_id][0];
        }
        if (!*ready_flag) {
            if (!channel_sender_alive(ch, is_a)) {
                irq_spin_unlock(&ch_lock, crit);
                return KERN_ERR_NOEXIST;
            }
            irq_spin_unlock(&ch_lock, crit);
            return KERN_ERR_TIMEOUT;
        }
    }

    if (*cap_count_src > 0) {
        if (out_caps == NULL || out_cap_count == NULL) {
            irq_spin_unlock(&ch_lock, crit);
            return KERN_ERR_RESOURCE;
        }

        memcpy(out_caps, cap_src, sizeof(cap_id_t) * (*cap_count_src));
        *out_cap_count = *cap_count_src;
    } else if (out_cap_count != NULL) {
        *out_cap_count = 0;
    }

    /* 读取消息 */
    memcpy(msg, src_buf, ch->msg_size);
    *ready_flag = 0;
    *cap_count_src = 0;
    memset(cap_src, 0, sizeof(cap_id_t) * IPC_CAPS_MAX);

    /* 如果对应方向有发送者等待槽位，唤醒一个 */
    channel_wake_send_waiter(send_wq, recv_wq, src_buf, ready_flag, cap_src,
                             cap_count_src, is_a ? ch->peer_a : ch->peer_b);

    irq_spin_unlock(&ch_lock, crit);
    return KERN_OK;
}

kern_err_t channel_recv(ch_id_t ch_id, void *msg, uint32_t timeout) {
    return channel_recv_common(ch_id, msg, NULL, NULL, timeout);
}

#if SYSCALL_ENABLE
kern_err_t channel_recv_syscall(ch_id_t ch_id, void *user_msg,
                                uint32_t timeout) {
    if (hal_irq_get_active() >= 0) return KERN_ERR_ISR;

    uint32_t crit = irq_spin_lock(&ch_lock);

    channel_t *ch = ch_get(ch_id);
    if (ch == NULL || user_msg == NULL) {
        irq_spin_unlock(&ch_lock, crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();
    if (current == NULL ||
        current->id < 0 || current->id >= KERNEL_MAX_TASKS) {
        irq_spin_unlock(&ch_lock, crit);
        return KERN_ERR_STATE;
    }
    trace_record(TRACE_IPC_RECV, (uint8_t)current->id, (uint16_t)ch_id);

    int is_a = 0;
    kern_err_t side_err = channel_get_recv_side(ch, current, &is_a);
    if (side_err != KERN_OK) {
        irq_spin_unlock(&ch_lock, crit);
        return side_err;
    }

    uint8_t *src_buf;
    uint8_t *ready_flag;
    wait_queue_t *recv_wq;
    wait_queue_t *send_wq;
    cap_id_t *cap_src;
    uint8_t *cap_count_src;

    if (is_a) {
        src_buf = ch->b_to_a_buf;
        ready_flag = &ch->b_to_a_ready;
        recv_wq = &ch->a_recv_waiters;
        send_wq = &ch->b_send_waiters;
        cap_src = ch_cap_id_buffers[ch_id][1];
        cap_count_src = &ch_cap_count_buffers[ch_id][1];
    } else {
        src_buf = ch->a_to_b_buf;
        ready_flag = &ch->a_to_b_ready;
        recv_wq = &ch->b_recv_waiters;
        send_wq = &ch->a_send_waiters;
        cap_src = ch_cap_id_buffers[ch_id][0];
        cap_count_src = &ch_cap_count_buffers[ch_id][0];
    }

    if (!*ready_flag) {
        if (!channel_sender_alive(ch, is_a)) {
            irq_spin_unlock(&ch_lock, crit);
            return KERN_ERR_NOEXIST;
        }
        if (timeout == 0) {
            irq_spin_unlock(&ch_lock, crit);
            return KERN_ERR_TIMEOUT;
        }

        ch_syscall_recv_msg[current->id] = user_msg;
        current->syscall_blocked = 1;
        current->block_reason = BLOCK_REASON_CH_RECV;
        current->block_obj = ch;
        current->block_result = KERN_OK;
        wait_queue_add(recv_wq, current);
        {
            extern void sched_remove_ready(tcb_t *tcb);
            sched_remove_ready(current);
        }
        current->state = TASK_STATE_BLOCKED;
        if (timeout > 0) {
            extern uint32_t sched_get_tick_count(void);
            current->wake_tick = sched_get_tick_count() + timeout;
        } else {
            current->wake_tick = 0;
        }

        irq_spin_unlock(&ch_lock, crit);
        return KERN_SYSCALL_BLOCKED;
    }

    if (*cap_count_src > 0) {
        irq_spin_unlock(&ch_lock, crit);
        return KERN_ERR_RESOURCE;
    }

    memcpy(user_msg, src_buf, ch->msg_size);
    *ready_flag = 0;
    *cap_count_src = 0;
    memset(cap_src, 0, sizeof(cap_id_t) * IPC_CAPS_MAX);

    channel_wake_send_waiter(send_wq, recv_wq, src_buf, ready_flag, cap_src,
                             cap_count_src, is_a ? ch->peer_a : ch->peer_b);

    irq_spin_unlock(&ch_lock, crit);
    return KERN_OK;
}

kern_err_t channel_recv_caps_syscall(ch_id_t ch_id,
                                     void *user_msg,
                                     cap_id_t *out_caps,
                                     uint8_t *out_cap_count,
                                     uint32_t timeout) {
    if (hal_irq_get_active() >= 0) return KERN_ERR_ISR;
    if (user_msg == NULL || out_caps == NULL || out_cap_count == NULL) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = irq_spin_lock(&ch_lock);

    channel_t *ch = ch_get(ch_id);
    if (ch == NULL) {
        irq_spin_unlock(&ch_lock, crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();
    if (current == NULL ||
        current->id < 0 || current->id >= KERNEL_MAX_TASKS) {
        irq_spin_unlock(&ch_lock, crit);
        return KERN_ERR_STATE;
    }
    trace_record(TRACE_IPC_RECV, (uint8_t)current->id, (uint16_t)ch_id);

    int is_a = 0;
    kern_err_t side_err = channel_get_recv_side(ch, current, &is_a);
    if (side_err != KERN_OK) {
        irq_spin_unlock(&ch_lock, crit);
        return side_err;
    }

    uint8_t *src_buf;
    uint8_t *ready_flag;
    wait_queue_t *recv_wq;
    wait_queue_t *send_wq;
    cap_id_t *cap_src;
    uint8_t *cap_count_src;

    if (is_a) {
        src_buf = ch->b_to_a_buf;
        ready_flag = &ch->b_to_a_ready;
        recv_wq = &ch->a_recv_waiters;
        send_wq = &ch->b_send_waiters;
        cap_src = ch_cap_id_buffers[ch_id][1];
        cap_count_src = &ch_cap_count_buffers[ch_id][1];
    } else {
        src_buf = ch->a_to_b_buf;
        ready_flag = &ch->a_to_b_ready;
        recv_wq = &ch->b_recv_waiters;
        send_wq = &ch->a_send_waiters;
        cap_src = ch_cap_id_buffers[ch_id][0];
        cap_count_src = &ch_cap_count_buffers[ch_id][0];
    }

    if (!*ready_flag) {
        if (!channel_sender_alive(ch, is_a)) {
            irq_spin_unlock(&ch_lock, crit);
            return KERN_ERR_NOEXIST;
        }
        if (timeout == 0) {
            irq_spin_unlock(&ch_lock, crit);
            return KERN_ERR_TIMEOUT;
        }

        ch_syscall_recv_msg[current->id] = user_msg;
        ch_syscall_recv_caps[current->id] = out_caps;
        ch_syscall_recv_cap_count[current->id] = out_cap_count;
        current->syscall_blocked = 1;
        current->block_reason = BLOCK_REASON_CH_RECV;
        current->block_obj = ch;
        current->block_result = KERN_OK;
        wait_queue_add(recv_wq, current);
        {
            extern void sched_remove_ready(tcb_t *tcb);
            sched_remove_ready(current);
        }
        current->state = TASK_STATE_BLOCKED;
        if (timeout > 0) {
            extern uint32_t sched_get_tick_count(void);
            current->wake_tick = sched_get_tick_count() + timeout;
        } else {
            current->wake_tick = 0;
        }

        irq_spin_unlock(&ch_lock, crit);
        return KERN_SYSCALL_BLOCKED;
    }

    if (*cap_count_src > 0) {
        memcpy(out_caps, cap_src, sizeof(cap_id_t) * (*cap_count_src));
        *out_cap_count = *cap_count_src;
    } else {
        *out_cap_count = 0;
    }

    memcpy(user_msg, src_buf, ch->msg_size);
    *ready_flag = 0;
    *cap_count_src = 0;
    memset(cap_src, 0, sizeof(cap_id_t) * IPC_CAPS_MAX);

    channel_wake_send_waiter(send_wq, recv_wq, src_buf, ready_flag, cap_src,
                             cap_count_src, is_a ? ch->peer_a : ch->peer_b);

    irq_spin_unlock(&ch_lock, crit);
    return KERN_OK;
}
#endif

kern_err_t channel_recv_caps(ch_id_t ch_id,
                             void *msg,
                             cap_id_t *out_caps,
                             uint8_t *out_cap_count,
                             uint32_t timeout) {
    return channel_recv_common(ch_id, msg, out_caps, out_cap_count, timeout);
}

void *channel_get_shm(ch_id_t ch_id) {
    channel_t *ch = ch_get(ch_id);
    return ch ? ch->shm : NULL;
}

#endif /* IPC_CHANNEL */
