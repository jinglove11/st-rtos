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
#include "trace.h"
#include <string.h>

#if IPC_CHANNEL

/*============================================================================
 * 静态分配
 *============================================================================*/

typedef struct {
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

/* 消消息缓冲区: [channel][direction 0=a_to_b, 1=b_to_a][msg_size] */
static uint8_t ch_msg_buffers[KERN_MAX_CHANNELS][2][KERN_CH_MSG_SIZE]
    __attribute__((aligned(4)));

static cap_id_t ch_cap_id_buffers[KERN_MAX_CHANNELS][2][IPC_CAPS_MAX];
static uint8_t ch_cap_count_buffers[KERN_MAX_CHANNELS][2];

/* 共享内存池 */
static uint8_t ch_shm_pool[KERN_MAX_CHANNELS][256]
    __attribute__((aligned(8)));

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

/*============================================================================
 * 公开接口
 *============================================================================*/

void channel_init(void) {
    memset(ch_pool, 0, sizeof(ch_pool));
    memset(ch_cap_id_buffers, 0, sizeof(ch_cap_id_buffers));
    memset(ch_cap_count_buffers, 0, sizeof(ch_cap_count_buffers));
    ch_used_bitmap = 0;
}

ch_id_t channel_create(uint16_t msg_size, uint32_t shm_size) {
    if (msg_size == 0 || msg_size > KERN_CH_MSG_SIZE) return KERN_INVALID_ID;

    uint32_t crit = hal_irq_save();

    ch_id_t id = alloc_ch_id();
    if (id == KERN_INVALID_ID) {
        hal_irq_restore(crit);
        return KERN_INVALID_ID;
    }

    channel_t *ch = &ch_pool[id];
    memset(ch, 0, sizeof(channel_t));

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

    hal_irq_restore(crit);
    return id;
}

kern_err_t channel_delete(ch_id_t ch_id) {
    uint32_t crit = hal_irq_save();

    channel_t *ch = ch_get(ch_id);
    if (ch == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    /* 唤醒所有等待的任务 */
    channel_wake_all(&ch->a_recv_waiters, KERN_ERR_NOEXIST);
    channel_wake_all(&ch->b_recv_waiters, KERN_ERR_NOEXIST);
    channel_wake_all(&ch->a_send_waiters, KERN_ERR_NOEXIST);
    channel_wake_all(&ch->b_send_waiters, KERN_ERR_NOEXIST);
    memset(ch_cap_id_buffers[ch_id], 0, sizeof(ch_cap_id_buffers[ch_id]));
    memset(ch_cap_count_buffers[ch_id], 0, sizeof(ch_cap_count_buffers[ch_id]));

    memset(ch, 0, sizeof(channel_t));
    free_ch_id(ch_id);

    hal_irq_restore(crit);
    return KERN_OK;
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

    if (tcb->id == ch->peer_a) {
        ch->peer_a = KERN_INVALID_ID;
    }
    if (tcb->id == ch->peer_b) {
        ch->peer_b = KERN_INVALID_ID;
    }
}

kern_err_t channel_connect(ch_id_t ch_id, task_id_t peer_a, task_id_t peer_b) {
    uint32_t crit = hal_enter_critical();

    channel_t *ch = ch_get(ch_id);
    if (ch == NULL) {
        hal_exit_critical(crit);
        return KERN_ERR_PARAM;
    }

    ch->peer_a = peer_a;
    ch->peer_b = peer_b;

    hal_exit_critical(crit);
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

    uint32_t crit = hal_enter_critical();

    channel_t *ch = ch_get(ch_id);
    if (ch == NULL) {
        hal_exit_critical(crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();
    trace_record(TRACE_IPC_SEND, (uint8_t)current->id, (uint16_t)ch_id);
    int is_a = 0;
    kern_err_t side_err = channel_get_side(ch, current, &is_a);
    if (side_err != KERN_OK) {
        hal_exit_critical(crit);
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
            hal_exit_critical(crit);
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

        hal_exit_critical(crit);
        hal_trigger_pendsv();

        while (current->state == TASK_STATE_BLOCKED) {
            __asm volatile("wfi");
            __asm volatile("dmb");
        }

        if (current->block_result != KERN_OK) {
            crit = hal_enter_critical();
            if (current->block_obj == ch) {
                wait_queue_remove_safe(send_wq, current);
                current->block_obj = NULL;
            }
            hal_exit_critical(crit);
            return current->block_result;
        }

        crit = hal_enter_critical();
        ch = ch_get(ch_id);
        if (ch == NULL) {
            hal_exit_critical(crit);
            return KERN_ERR_NOEXIST;
        }

        side_err = channel_get_side(ch, current, &is_a);
        if (side_err != KERN_OK) {
            hal_exit_critical(crit);
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
            hal_exit_critical(crit);
            return KERN_ERR_BUSY;
        }
    }

    if (cap_count > 0) {
        tcb_t *receiver = task_get_tcb(receiver_id);
        kern_err_t xfer_err = ipc_transfer_caps(current, receiver,
                                                caps, cap_count, cap_dst);
        if (xfer_err != KERN_OK) {
            hal_exit_critical(crit);
            return xfer_err;
        }
    }

    /* 写入消息 */
    memcpy(dst_buf, msg, ch->msg_size);
    *cap_count_dst = cap_count;
    *ready_flag = 1;

    /* 如果对端在等待接收，唤醒它 */
    if (recv_wq->count > 0) {
        tcb_t *waiter = wait_queue_get_highest(recv_wq);
        if (waiter) {
            wait_queue_remove(recv_wq, waiter);
            waiter->block_result = KERN_OK;
            sched_wakeup(waiter, KERN_OK);
        }
    }

    hal_exit_critical(crit);
    return KERN_OK;
}

kern_err_t channel_send(ch_id_t ch_id, const void *msg, uint32_t timeout) {
    return channel_send_common(ch_id, msg, NULL, 0, timeout);
}

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

    uint32_t crit = hal_enter_critical();

    channel_t *ch = ch_get(ch_id);
    if (ch == NULL) {
        hal_exit_critical(crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();
    trace_record(TRACE_IPC_RECV, (uint8_t)current->id, (uint16_t)ch_id);
    int is_a = 0;
    kern_err_t side_err = channel_get_side(ch, current, &is_a);
    if (side_err != KERN_OK) {
        hal_exit_critical(crit);
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
        if (timeout == 0) {
            hal_exit_critical(crit);
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

        hal_exit_critical(crit);
        hal_trigger_pendsv();

        while (current->state == TASK_STATE_BLOCKED) {
            __asm volatile("wfi");
            __asm volatile("dmb");
        }

        if (current->block_result != KERN_OK) {
            crit = hal_enter_critical();
            if (current->block_obj == ch) {
                wait_queue_remove_safe(recv_wq, current);
                current->block_obj = NULL;
            }
            hal_exit_critical(crit);
            return current->block_result;
        }

        crit = hal_enter_critical();
        ch = ch_get(ch_id);
        if (ch == NULL) {
            hal_exit_critical(crit);
            return KERN_ERR_NOEXIST;
        }

        side_err = channel_get_side(ch, current, &is_a);
        if (side_err != KERN_OK) {
            hal_exit_critical(crit);
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
            hal_exit_critical(crit);
            return KERN_ERR_TIMEOUT;
        }
    }

    if (*cap_count_src > 0) {
        if (out_caps == NULL || out_cap_count == NULL) {
            hal_exit_critical(crit);
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
    if (send_wq->count > 0) {
        tcb_t *waiter = wait_queue_get_highest(send_wq);
        if (waiter) {
            wait_queue_remove(send_wq, waiter);
            waiter->block_result = KERN_OK;
            sched_wakeup(waiter, KERN_OK);
        }
    }

    hal_exit_critical(crit);
    return KERN_OK;
}

kern_err_t channel_recv(ch_id_t ch_id, void *msg, uint32_t timeout) {
    return channel_recv_common(ch_id, msg, NULL, NULL, timeout);
}

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
