/**
 * @file endpoint.c
 * @brief Endpoint (C/S) 消息传递实现
 *
 * 多对一模型：多个客户端发送请求，服务端接收并回复。
 * 客户端 send → 阻塞等待回复
 * 服务端 recv → 处理 → reply → 唤醒客户端
 */

#include "endpoint.h"
#include "ipc_transfer.h"
#include "wait_queue.h"
#include "scheduler.h"
#include "kernel_config.h"
#include "kernel_types.h"
#include "hal.h"
#include "trace.h"
#include <string.h>

#if IPC_ENDPOINT

/*============================================================================
 * 静态分配
 *============================================================================*/

typedef struct {
    char        name[ENDPOINT_NAME_LEN];
    uint16_t    msg_size;
    uint16_t    max_pending;
    uint16_t    pending_count;

    wait_queue_t recv_waiters;      // 服务端等待接收
    wait_queue_t send_waiters;      // 客户端等待发送槽位
    wait_queue_t reply_waiters;     // 客户端等待回复

    /* 环形缓冲区 */
    uint8_t    *msg_buf;            // [max_pending][msg_size]
    tcb_t     **sender_buf;         // [max_pending] 发送者 TCB
    uint16_t    head;
    uint16_t    tail;
    uint16_t    count;

    uint32_t    next_request_gen;  // request generation

    uint8_t     in_use;
} endpoint_t;

static endpoint_t ep_pool[KERN_MAX_ENDPOINTS];
static uint32_t ep_used_bitmap;

/* 消息缓冲区: [endpoint][slot][msg_size] */
static uint8_t ep_msg_buffers[KERN_MAX_ENDPOINTS]
                              [KERN_EP_MAX_PENDING]
                              [KERN_EP_MSG_SIZE]
    __attribute__((aligned(4)));

/* 发送者指针缓冲区: [endpoint][slot] */
static tcb_t *ep_sender_buffers[KERN_MAX_ENDPOINTS][KERN_EP_MAX_PENDING];

/* 每任务: 客户端的 msg 指针 (用于 reply 写回) */
static void *ep_client_msg[KERNEL_MAX_TASKS];
static uint32_t ep_client_gen[KERNEL_MAX_TASKS];
static tcb_t *ep_server_sender[KERN_MAX_ENDPOINTS][KERNEL_MAX_TASKS];
static uint32_t ep_server_gen[KERN_MAX_ENDPOINTS][KERNEL_MAX_TASKS];
static uint8_t ep_server_dead[KERN_MAX_ENDPOINTS][KERNEL_MAX_TASKS];
static uint32_t ep_request_gen_buffers[KERN_MAX_ENDPOINTS][KERN_EP_MAX_PENDING];
static ipc_cap_xfer_t ep_cap_xfer_buffers[KERN_MAX_ENDPOINTS]
                                          [KERN_EP_MAX_PENDING]
                                          [IPC_CAPS_MAX];
static uint8_t ep_cap_count_buffers[KERN_MAX_ENDPOINTS][KERN_EP_MAX_PENDING];

/*============================================================================
 * 内部函数
 *============================================================================*/

static ep_id_t alloc_ep_id(void) {
    for (int i = 0; i < KERN_MAX_ENDPOINTS; i++) {
        if (!(ep_used_bitmap & (1U << i))) {
            ep_used_bitmap |= (1U << i);
            return (ep_id_t)i;
        }
    }
    return KERN_INVALID_ID;
}

static void free_ep_id(ep_id_t id) {
    if (id >= 0 && id < KERN_MAX_ENDPOINTS) {
        ep_used_bitmap &= ~(1U << id);
    }
}

static endpoint_t *ep_get(ep_id_t id) {
    if (id < 0 || id >= KERN_MAX_ENDPOINTS) return NULL;
    if (!ep_pool[id].in_use) return NULL;
    return &ep_pool[id];
}

static void endpoint_clear_server_bindings(ep_id_t ep_id, tcb_t *sender) {
    if (ep_id < 0 || ep_id >= KERN_MAX_ENDPOINTS || sender == NULL) {
        return;
    }

    for (task_id_t tid = 0; tid < KERNEL_MAX_TASKS; tid++) {
        if (ep_server_sender[ep_id][tid] == sender) {
            ep_server_sender[ep_id][tid] = NULL;
            ep_server_gen[ep_id][tid] = 0;
            ep_server_dead[ep_id][tid] = 1;
        }
    }
}

static void endpoint_cancel_sender(ep_id_t ep_id, endpoint_t *ep, tcb_t *sender) {
    if (ep == NULL || sender == NULL) {
        return;
    }

    wait_queue_remove_safe(&ep->send_waiters, sender);
    wait_queue_remove_safe(&ep->reply_waiters, sender);

    uint16_t kept = 0;
    for (uint16_t i = 0; i < ep->count; i++) {
        uint16_t src_idx = (uint16_t)((ep->tail + i) % ep->max_pending);
        if (ep->sender_buf[src_idx] == sender) {
            if (ep->pending_count > 0) {
                ep->pending_count--;
            }
            ep->sender_buf[src_idx] = NULL;
            ep_cap_count_buffers[ep_id][src_idx] = 0;
            ep_request_gen_buffers[ep_id][src_idx] = 0;
            continue;
        }

        uint16_t dst_idx = (uint16_t)((ep->tail + kept) % ep->max_pending);
        if (dst_idx != src_idx) {
            memcpy(ep->msg_buf + (dst_idx * ep->msg_size),
                   ep->msg_buf + (src_idx * ep->msg_size),
                   ep->msg_size);
            ep->sender_buf[dst_idx] = ep->sender_buf[src_idx];
            ep->sender_buf[src_idx] = NULL;
            ep_cap_count_buffers[ep_id][dst_idx] =
                ep_cap_count_buffers[ep_id][src_idx];
            ep_request_gen_buffers[ep_id][dst_idx] =
                ep_request_gen_buffers[ep_id][src_idx];
            memcpy(ep_cap_xfer_buffers[ep_id][dst_idx],
                   ep_cap_xfer_buffers[ep_id][src_idx],
                   sizeof(ep_cap_xfer_buffers[ep_id][dst_idx]));
            ep_cap_count_buffers[ep_id][src_idx] = 0;
            ep_request_gen_buffers[ep_id][src_idx] = 0;
        }
        kept++;
    }

    ep->count = kept;
    ep->head = (uint16_t)((ep->tail + kept) % ep->max_pending);
    endpoint_clear_server_bindings(ep_id, sender);

    if (sender->id >= 0 && sender->id < KERNEL_MAX_TASKS) {
        ep_client_msg[sender->id] = NULL;
        ep_client_gen[sender->id] = 0;
    }
}

/*============================================================================
 * 公开接口
 *============================================================================*/

void endpoint_init(void) {
    memset(ep_pool, 0, sizeof(ep_pool));
    ep_used_bitmap = 0;
    memset(ep_client_msg, 0, sizeof(ep_client_msg));
    memset(ep_client_gen, 0, sizeof(ep_client_gen));
    memset(ep_server_sender, 0, sizeof(ep_server_sender));
    memset(ep_server_gen, 0, sizeof(ep_server_gen));
    memset(ep_server_dead, 0, sizeof(ep_server_dead));
    memset(ep_request_gen_buffers, 0, sizeof(ep_request_gen_buffers));
    memset(ep_cap_xfer_buffers, 0, sizeof(ep_cap_xfer_buffers));
    memset(ep_cap_count_buffers, 0, sizeof(ep_cap_count_buffers));
}

ep_id_t endpoint_create(const char *name, uint16_t msg_size, uint16_t max_pending) {
    if (msg_size == 0 || msg_size > KERN_EP_MSG_SIZE) return KERN_INVALID_ID;
    if (max_pending == 0 || max_pending > KERN_EP_MAX_PENDING) return KERN_INVALID_ID;

    uint32_t crit = hal_irq_save();

    ep_id_t id = alloc_ep_id();
    if (id == KERN_INVALID_ID) {
        hal_irq_restore(crit);
        return KERN_INVALID_ID;
    }

    endpoint_t *ep = &ep_pool[id];
    memset(ep, 0, sizeof(endpoint_t));

    if (name) {
        strncpy(ep->name, name, ENDPOINT_NAME_LEN - 1);
        ep->name[ENDPOINT_NAME_LEN - 1] = '\0';
    }

    ep->msg_size     = msg_size;
    ep->max_pending  = max_pending;
    ep->pending_count = 0;
    ep->msg_buf      = &ep_msg_buffers[id][0][0];
    ep->sender_buf   = ep_sender_buffers[id];
    ep->head         = 0;
    ep->tail         = 0;
    ep->count        = 0;
    ep->next_request_gen = 1;
    ep->in_use       = 1;

    wait_queue_init(&ep->recv_waiters);
    wait_queue_init(&ep->send_waiters);
    wait_queue_init(&ep->reply_waiters);

    hal_irq_restore(crit);
    return id;
}

kern_err_t endpoint_delete(ep_id_t ep_id) {
    uint32_t crit = hal_irq_save();

    endpoint_t *ep = ep_get(ep_id);
    if (ep == NULL) {
        hal_irq_restore(crit);
        return KERN_ERR_PARAM;
    }

    /* 唤醒所有等待的服务端 */
    tcb_t *tcb = ep->recv_waiters.head;
    while (tcb) {
        tcb_t *next = tcb->wait_next;
        tcb->wait_next = NULL;
        tcb->wait_prev = NULL;
        tcb->block_result = KERN_ERR_NOEXIST;
        sched_wakeup(tcb, KERN_ERR_NOEXIST);
        tcb = next;
    }

    /* 唤醒所有等待发送槽位的客户端 */
    tcb = ep->send_waiters.head;
    while (tcb) {
        tcb_t *next = tcb->wait_next;
        tcb->wait_next = NULL;
        tcb->wait_prev = NULL;
        tcb->block_result = KERN_ERR_NOEXIST;
        sched_wakeup(tcb, KERN_ERR_NOEXIST);
        tcb = next;
    }

    /* 唤醒所有等待回复的客户端 */
    tcb = ep->reply_waiters.head;
    while (tcb) {
        tcb_t *next = tcb->wait_next;
        tcb->wait_next = NULL;
        tcb->wait_prev = NULL;
        tcb->block_result = KERN_ERR_NOEXIST;
        sched_wakeup(tcb, KERN_ERR_NOEXIST);
        tcb = next;
    }

    memset(ep_server_sender[ep_id], 0, sizeof(ep_server_sender[ep_id]));
    memset(ep_server_gen[ep_id], 0, sizeof(ep_server_gen[ep_id]));
    memset(ep_server_dead[ep_id], 0, sizeof(ep_server_dead[ep_id]));
    memset(ep_request_gen_buffers[ep_id], 0, sizeof(ep_request_gen_buffers[ep_id]));
    memset(ep_cap_xfer_buffers[ep_id], 0, sizeof(ep_cap_xfer_buffers[ep_id]));
    memset(ep_cap_count_buffers[ep_id], 0, sizeof(ep_cap_count_buffers[ep_id]));
    memset(ep, 0, sizeof(endpoint_t));
    free_ep_id(ep_id);

    hal_irq_restore(crit);
    return KERN_OK;
}

void endpoint_cleanup_task(void *endpoint_obj, tcb_t *tcb) {
    endpoint_t *ep = (endpoint_t *)endpoint_obj;

    if (ep == NULL || tcb == NULL) {
        return;
    }

    wait_queue_remove_safe(&ep->recv_waiters, tcb);
    wait_queue_remove_safe(&ep->send_waiters, tcb);
    wait_queue_remove_safe(&ep->reply_waiters, tcb);
    endpoint_cancel_sender((ep_id_t)(ep - ep_pool), ep, tcb);

    for (uint16_t i = 0; i < ep->max_pending; i++) {
        if (ep->sender_buf[i] == tcb) {
            ep->sender_buf[i] = NULL;
            ep_request_gen_buffers[(ep_id_t)(ep - ep_pool)][i] = 0;
        }
    }

    if (tcb->id >= 0 && tcb->id < KERNEL_MAX_TASKS) {
        ep_client_msg[tcb->id] = NULL;
        ep_client_gen[tcb->id] = 0;
    }
}

static kern_err_t endpoint_send_common(ep_id_t ep_id,
                                       void *msg,
                                       const ipc_cap_xfer_t *caps,
                                       uint8_t cap_count,
                                       uint32_t timeout) {
    if (hal_irq_get_active() >= 0) return KERN_ERR_ISR;
    if (cap_count > IPC_CAPS_MAX) return KERN_ERR_PARAM;
    if (cap_count > 0 && caps == NULL) return KERN_ERR_PARAM;

    uint32_t crit = hal_enter_critical();

    endpoint_t *ep = ep_get(ep_id);
    if (ep == NULL) {
        hal_exit_critical(crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();
    trace_record(TRACE_IPC_SEND, (uint8_t)current->id, (uint16_t)ep_id);

    /* 如果缓冲区已满，等待服务端腾出空间 */
    if (ep->count >= ep->max_pending) {
        if (timeout == 0) {
            hal_exit_critical(crit);
            return KERN_ERR_TIMEOUT;
        }

        current->block_reason = BLOCK_REASON_EP_SEND;
        current->block_obj = ep;
        wait_queue_add(&ep->send_waiters, current);

        {
            extern void sched_remove_ready(tcb_t *tcb);
            sched_remove_ready(current);
        }
        current->state = TASK_STATE_BLOCKED;
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
            if (current->block_obj == ep) {
                wait_queue_remove_safe(&ep->send_waiters, current);
                current->block_obj = NULL;
            }
            hal_exit_critical(crit);
            return current->block_result;
        }

        /* 被唤醒后重新检查 */
        crit = hal_enter_critical();
        ep = ep_get(ep_id);
        if (ep == NULL) {
            hal_exit_critical(crit);
            return KERN_ERR_NOEXIST;
        }
    }

    /* 写入请求到环形缓冲区 */
    uint16_t slot = ep->head;
    uint8_t *dst = ep->msg_buf + (slot * ep->msg_size);
    memcpy(dst, msg, ep->msg_size);
    ep->sender_buf[slot] = current;
    uint32_t request_gen = ep->next_request_gen++;
    if (ep->next_request_gen == 0) {
        ep->next_request_gen = 1;
    }
    ep_request_gen_buffers[ep_id][slot] = request_gen;
    ep_cap_count_buffers[ep_id][slot] = cap_count;
    if (cap_count > 0) {
        memcpy(ep_cap_xfer_buffers[ep_id][slot],
               caps,
               (size_t)cap_count * sizeof(caps[0]));
    }
    ep->head = (ep->head + 1) % ep->max_pending;
    ep->count++;
    ep->pending_count++;

    /* 保存客户端 msg 指针，reply 时写回 */
    ep_client_msg[current->id] = msg;
    ep_client_gen[current->id] = request_gen;

    /* 如果有服务端在等待接收，唤醒它 */
    if (ep->recv_waiters.count > 0) {
        tcb_t *server = wait_queue_get_highest(&ep->recv_waiters);
        if (server) {
            wait_queue_remove(&ep->recv_waiters, server);
            server->block_result = KERN_OK;
            sched_wakeup(server, KERN_OK);
        }
    }

    /* 客户端挂入 reply_waiters，等待服务端回复 */
    wait_queue_add(&ep->reply_waiters, current);

    {
        extern void sched_remove_ready(tcb_t *tcb);
        sched_remove_ready(current);
    }
    current->state = TASK_STATE_BLOCKED;
    current->block_reason = BLOCK_REASON_EP_SEND;
    current->block_obj = ep;
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

    kern_err_t result = current->block_result;
    if (result != KERN_OK) {
        crit = hal_enter_critical();
        if (current->block_obj == ep) {
            endpoint_cancel_sender(ep_id, ep, current);
            current->block_obj = NULL;
        }
        hal_exit_critical(crit);
    }

    return result;
}

kern_err_t endpoint_send(ep_id_t ep_id, void *msg, uint32_t timeout) {
    return endpoint_send_common(ep_id, msg, NULL, 0, timeout);
}

kern_err_t endpoint_send_caps(ep_id_t ep_id,
                              void *msg,
                              const ipc_cap_xfer_t *caps,
                              uint8_t cap_count,
                              uint32_t timeout) {
    return endpoint_send_common(ep_id, msg, caps, cap_count, timeout);
}

static kern_err_t endpoint_recv_common(ep_id_t ep_id,
                                       void *msg,
                                       cap_id_t *out_caps,
                                       uint8_t *out_cap_count,
                                       uint32_t timeout) {
    if (hal_irq_get_active() >= 0) return KERN_ERR_ISR;

    uint32_t crit = hal_enter_critical();

    endpoint_t *ep = ep_get(ep_id);
    if (ep == NULL) {
        hal_exit_critical(crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();
    trace_record(TRACE_IPC_RECV, (uint8_t)current->id, (uint16_t)ep_id);

    /* 如果没有待处理消息，阻塞等待 */
    if (ep->count == 0) {
        if (timeout == 0) {
            hal_exit_critical(crit);
            return KERN_ERR_TIMEOUT;
        }

        current->block_reason = BLOCK_REASON_EP_RECV;
        current->block_obj = ep;
        wait_queue_add(&ep->recv_waiters, current);

        {
            extern void sched_remove_ready(tcb_t *tcb);
            sched_remove_ready(current);
        }
        current->state = TASK_STATE_BLOCKED;
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
            if (current->block_obj == ep) {
                wait_queue_remove(&ep->recv_waiters, current);
                current->block_obj = NULL;
            }
            hal_exit_critical(crit);
            return current->block_result;
        }

        crit = hal_enter_critical();
        ep = ep_get(ep_id);
        if (ep == NULL) {
            hal_exit_critical(crit);
            return KERN_ERR_NOEXIST;
        }
    }

    /* 从环形缓冲区取出消息 */
    uint16_t slot = ep->tail;
    uint8_t *src = ep->msg_buf + (slot * ep->msg_size);
    memcpy(msg, src, ep->msg_size);
    tcb_t *sender = ep->sender_buf[slot];
    uint32_t request_gen = ep_request_gen_buffers[ep_id][slot];
    if (current->id >= 0 && current->id < KERNEL_MAX_TASKS) {
        ep_server_dead[ep_id][current->id] = 0;
    }

    uint8_t cap_count = ep_cap_count_buffers[ep_id][slot];
    if (cap_count > 0) {
        if (out_caps == NULL || out_cap_count == NULL) {
            if (sender != NULL) {
                wait_queue_remove_safe(&ep->reply_waiters, sender);
                sender->block_result = KERN_ERR_RESOURCE;
                sched_wakeup(sender, KERN_ERR_RESOURCE);
            }
            ep_cap_count_buffers[ep_id][slot] = 0;
            ep->sender_buf[slot] = NULL;
            ep->tail = (ep->tail + 1) % ep->max_pending;
            ep->count--;
            ep->pending_count--;
            hal_exit_critical(crit);
            return KERN_ERR_RESOURCE;
        }

        kern_err_t xfer_err = ipc_transfer_caps(sender,
                                                current,
                                                ep_cap_xfer_buffers[ep_id][slot],
                                                cap_count,
                                                out_caps);
        if (xfer_err != KERN_OK) {
            if (sender != NULL) {
                wait_queue_remove_safe(&ep->reply_waiters, sender);
                sender->block_result = xfer_err;
                sched_wakeup(sender, xfer_err);
            }
            ep_cap_count_buffers[ep_id][slot] = 0;
            ep->sender_buf[slot] = NULL;
            ep->tail = (ep->tail + 1) % ep->max_pending;
            ep->count--;
            ep->pending_count--;
            hal_exit_critical(crit);
            return xfer_err;
        }
        *out_cap_count = cap_count;
    } else if (out_cap_count != NULL) {
        *out_cap_count = 0;
    }

    if (current->id >= 0 && current->id < KERNEL_MAX_TASKS) {
        ep_server_sender[ep_id][current->id] = sender;
        ep_server_gen[ep_id][current->id] = request_gen;
    }
    ep_cap_count_buffers[ep_id][slot] = 0;
    ep_request_gen_buffers[ep_id][slot] = 0;
    ep->tail = (ep->tail + 1) % ep->max_pending;
    ep->count--;
    ep->pending_count--;

    /* 如果有客户端在等待发送 (缓冲区满时)，唤醒一个 */
    {
        tcb_t *waiter = wait_queue_get_highest(&ep->send_waiters);
        if (waiter) {
            wait_queue_remove(&ep->send_waiters, waiter);
            waiter->block_result = KERN_OK;
            sched_wakeup(waiter, KERN_OK);
        }
    }

    hal_exit_critical(crit);
    return KERN_OK;
}

kern_err_t endpoint_recv(ep_id_t ep_id, void *msg, uint32_t timeout) {
    return endpoint_recv_common(ep_id, msg, NULL, NULL, timeout);
}

kern_err_t endpoint_recv_caps(ep_id_t ep_id,
                              void *msg,
                              cap_id_t *out_caps,
                              uint8_t *out_cap_count,
                              uint32_t timeout) {
    return endpoint_recv_common(ep_id, msg, out_caps, out_cap_count, timeout);
}

kern_err_t endpoint_reply(ep_id_t ep_id, const void *msg) {
    uint32_t crit = hal_enter_critical();

    endpoint_t *ep = ep_get(ep_id);
    if (ep == NULL) {
        hal_exit_critical(crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *server = sched_get_current();
    tcb_t *sender = NULL;
    uint32_t request_gen = 0;
    if (server != NULL && server->id >= 0 && server->id < KERNEL_MAX_TASKS) {
        sender = ep_server_sender[ep_id][server->id];
        request_gen = ep_server_gen[ep_id][server->id];
    }
    if (sender == NULL) {
        if (server != NULL && server->id >= 0 && server->id < KERNEL_MAX_TASKS &&
            ep_server_dead[ep_id][server->id] != 0) {
            ep_server_dead[ep_id][server->id] = 0;
            hal_exit_critical(crit);
            return KERN_ERR_NOEXIST;
        }
        hal_exit_critical(crit);
        return KERN_ERR_STATE;
    }
    if (sender->state != TASK_STATE_BLOCKED ||
        sender->block_obj != ep ||
        sender->block_reason != BLOCK_REASON_EP_SEND ||
        sender->id < 0 ||
        sender->id >= KERNEL_MAX_TASKS ||
        ep_client_gen[sender->id] != request_gen) {
        if (server != NULL && server->id >= 0 && server->id < KERNEL_MAX_TASKS) {
            ep_server_sender[ep_id][server->id] = NULL;
            ep_server_gen[ep_id][server->id] = 0;
            ep_server_dead[ep_id][server->id] = 1;
        }
        hal_exit_critical(crit);
        return KERN_ERR_NOEXIST;
    }

    /* 将回复写入客户端的原始 msg 缓冲区 */
    void *client_buf = ep_client_msg[sender->id];
    if (client_buf) {
        memcpy(client_buf, msg, ep->msg_size);
    }

    if (server != NULL && server->id >= 0 && server->id < KERNEL_MAX_TASKS) {
        ep_server_sender[ep_id][server->id] = NULL;
        ep_server_gen[ep_id][server->id] = 0;
        ep_server_dead[ep_id][server->id] = 0;
    }
    ep_client_msg[sender->id] = NULL;
    ep_client_gen[sender->id] = 0;

    /* 从 reply_waiters 移除并唤醒客户端 */
    wait_queue_remove_safe(&ep->reply_waiters, sender);
    sender->block_result = KERN_OK;
    sched_wakeup(sender, KERN_OK);

    hal_exit_critical(crit);
    return KERN_OK;
}

#endif /* IPC_ENDPOINT */
