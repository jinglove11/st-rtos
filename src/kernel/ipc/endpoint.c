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
#include "spinlock.h"
#include "trace.h"
#include "syscall.h"
#include "capability.h"
#include <string.h>

#if IPC_ENDPOINT

/*============================================================================
 * 静态分配
 *============================================================================*/

typedef struct {
    kobject_header_t hdr;            // M2-Step3b: 对象 header (generation 等)
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

    uint32_t    next_request_gen;  // request generation (per-IPC-request,非对象 generation)

    uint8_t     in_use;
} endpoint_t;

#if CAP_ENABLE
typedef struct {
    kobject_header_t hdr;       /* M2-Step3d: 对象 header */
    ep_id_t     ep_id;
    task_id_t   server_id;
    tcb_t      *sender;
    uint32_t    request_gen;
    cap_id_t    cap;
    uint8_t     active;
    uint8_t     used;
    kern_err_t  bind_error;
} endpoint_reply_t;
#endif

static endpoint_t ep_pool[KERN_MAX_ENDPOINTS];
static uint32_t ep_used_bitmap;
static irq_spinlock_t ep_lock; /* M1: SMP safe */

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
static void *ep_syscall_client_msg[KERNEL_MAX_TASKS];
static tcb_t *ep_server_sender[KERN_MAX_ENDPOINTS][KERNEL_MAX_TASKS];
static uint32_t ep_server_gen[KERN_MAX_ENDPOINTS][KERNEL_MAX_TASKS];
static uint8_t ep_server_dead[KERN_MAX_ENDPOINTS][KERNEL_MAX_TASKS];
static task_id_t ep_last_receiver[KERN_MAX_ENDPOINTS];
static uint32_t ep_last_receiver_gen[KERN_MAX_ENDPOINTS];
static void *ep_syscall_recv_msg[KERNEL_MAX_TASKS];
static cap_id_t *ep_syscall_recv_caps[KERNEL_MAX_TASKS];
static uint8_t *ep_syscall_recv_cap_count[KERNEL_MAX_TASKS];
static uint32_t ep_request_gen_buffers[KERN_MAX_ENDPOINTS][KERN_EP_MAX_PENDING];
static ipc_cap_xfer_t ep_cap_xfer_buffers[KERN_MAX_ENDPOINTS]
                                          [KERN_EP_MAX_PENDING]
                                          [IPC_CAPS_MAX];
static uint8_t ep_cap_count_buffers[KERN_MAX_ENDPOINTS][KERN_EP_MAX_PENDING];

#if CAP_ENABLE
static endpoint_reply_t ep_reply_objects[KERN_MAX_ENDPOINTS][KERNEL_MAX_TASKS];
static cap_id_t ep_server_reply_cap[KERN_MAX_ENDPOINTS][KERNEL_MAX_TASKS];
#endif

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

#if CAP_ENABLE
static void endpoint_invalidate_reply_cap(ep_id_t ep_id, task_id_t server_id) {
    if (ep_id < 0 || ep_id >= KERN_MAX_ENDPOINTS ||
        server_id < 0 || server_id >= KERNEL_MAX_TASKS) {
        return;
    }

    cap_id_t cap = ep_server_reply_cap[ep_id][server_id];
    ep_server_reply_cap[ep_id][server_id] = KERN_INVALID_ID;
    ep_reply_objects[ep_id][server_id].active = 0;
    ep_reply_objects[ep_id][server_id].used = 1;
    ep_reply_objects[ep_id][server_id].sender = NULL;
    ep_reply_objects[ep_id][server_id].request_gen = 0;
    ep_reply_objects[ep_id][server_id].bind_error = KERN_OK;
    if (cap != KERN_INVALID_ID) {
        cap_delete(cap);
    }
}

static void endpoint_invalidate_sender_reply_caps(ep_id_t ep_id, tcb_t *sender) {
    if (ep_id < 0 || ep_id >= KERN_MAX_ENDPOINTS || sender == NULL) {
        return;
    }

    for (task_id_t tid = 0; tid < KERNEL_MAX_TASKS; tid++) {
        if (ep_reply_objects[ep_id][tid].sender == sender) {
            endpoint_invalidate_reply_cap(ep_id, tid);
        }
    }
}

static void endpoint_bind_reply_cap(ep_id_t ep_id, tcb_t *server,
                                    tcb_t *sender, uint32_t request_gen) {
    if (ep_id < 0 || ep_id >= KERN_MAX_ENDPOINTS ||
        server == NULL || sender == NULL ||
        server->id < 0 || server->id >= KERNEL_MAX_TASKS) {
        return;
    }

    endpoint_invalidate_reply_cap(ep_id, server->id);

    endpoint_reply_t *reply = &ep_reply_objects[ep_id][server->id];
    /* M2-Step3d: 每次 bind 都 bump generation,使上次的 reply cap 失效
     * (上次的 reply cap 是一次性的,bind 新 request 后旧的应拒绝)。 */
    reply->hdr.obj_type   = CAP_OBJ_REPLY;
    reply->hdr.generation = kobj_header_prepare_reuse(&reply->hdr);
    reply->ep_id = ep_id;
    reply->server_id = server->id;
    reply->sender = sender;
    reply->request_gen = request_gen;
    reply->active = 1;
    reply->used = 0;
    reply->bind_error = KERN_OK;
    reply->cap = cap_create_for_gen(server, reply, CAP_OBJ_REPLY, CAP_WRITE,
                                    reply->hdr.generation);
    if (reply->cap == KERN_INVALID_ID) {
        reply->active = 0;
        reply->used = 1;
        reply->sender = NULL;
        reply->request_gen = 0;
        reply->bind_error = KERN_ERR_RESOURCE;
    }
    ep_server_reply_cap[ep_id][server->id] = reply->cap;
}
#endif

static void endpoint_clear_server_bindings(ep_id_t ep_id, tcb_t *sender) {
    if (ep_id < 0 || ep_id >= KERN_MAX_ENDPOINTS || sender == NULL) {
        return;
    }

    for (task_id_t tid = 0; tid < KERNEL_MAX_TASKS; tid++) {
        if (ep_server_sender[ep_id][tid] == sender) {
            ep_server_sender[ep_id][tid] = NULL;
            ep_server_gen[ep_id][tid] = 0;
            ep_server_dead[ep_id][tid] = 1;
#if CAP_ENABLE
            endpoint_invalidate_reply_cap(ep_id, tid);
#endif
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
        ep_syscall_client_msg[sender->id] = NULL;
    }
}

/*============================================================================
 * 公开接口
 *============================================================================*/

void endpoint_init(void) {
    irq_spin_init(&ep_lock);
    memset(ep_pool, 0, sizeof(ep_pool));
    ep_used_bitmap = 0;
    memset(ep_client_msg, 0, sizeof(ep_client_msg));
    memset(ep_client_gen, 0, sizeof(ep_client_gen));
    memset(ep_syscall_client_msg, 0, sizeof(ep_syscall_client_msg));
    memset(ep_server_sender, 0, sizeof(ep_server_sender));
    memset(ep_server_gen, 0, sizeof(ep_server_gen));
    memset(ep_server_dead, 0, sizeof(ep_server_dead));
    for (ep_id_t ep = 0; ep < KERN_MAX_ENDPOINTS; ep++) {
        ep_last_receiver[ep] = KERN_INVALID_ID;
        ep_last_receiver_gen[ep] = 0;
    }
    memset(ep_syscall_recv_msg, 0, sizeof(ep_syscall_recv_msg));
    memset(ep_syscall_recv_caps, 0, sizeof(ep_syscall_recv_caps));
    memset(ep_syscall_recv_cap_count, 0, sizeof(ep_syscall_recv_cap_count));
    memset(ep_request_gen_buffers, 0, sizeof(ep_request_gen_buffers));
    memset(ep_cap_xfer_buffers, 0, sizeof(ep_cap_xfer_buffers));
    memset(ep_cap_count_buffers, 0, sizeof(ep_cap_count_buffers));
#if CAP_ENABLE
    memset(ep_reply_objects, 0, sizeof(ep_reply_objects));
    for (ep_id_t ep = 0; ep < KERN_MAX_ENDPOINTS; ep++) {
        for (task_id_t tid = 0; tid < KERNEL_MAX_TASKS; tid++) {
            ep_server_reply_cap[ep][tid] = KERN_INVALID_ID;
        }
    }
#endif
}

ep_id_t endpoint_create(const char *name, uint16_t msg_size, uint16_t max_pending) {
    if (msg_size == 0 || msg_size > KERN_EP_MSG_SIZE) return KERN_INVALID_ID;
    if (max_pending == 0 || max_pending > KERN_EP_MAX_PENDING) return KERN_INVALID_ID;

    uint32_t crit = irq_spin_lock(&ep_lock);

    ep_id_t id = alloc_ep_id();
    if (id == KERN_INVALID_ID) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_INVALID_ID;
    }

    endpoint_t *ep = &ep_pool[id];
    /* M2-Step3b: 跨 memset 保留 generation (endpoint_delete 已 bump)。
     * 首次分配 generation=0 → 初始化为 1; 复用时保留 bumped 值。 */
    uint16_t saved_gen = ep->hdr.generation;
    memset(ep, 0, sizeof(endpoint_t));
    kobj_header_init(&ep->hdr, CAP_OBJ_ENDPOINT);
    if (saved_gen != 0) {
        ep->hdr.generation = saved_gen;
    }

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

    irq_spin_unlock(&ep_lock, crit);
    return id;
}

uint16_t endpoint_msg_size(ep_id_t ep_id) {
    uint32_t crit = irq_spin_lock(&ep_lock);
    endpoint_t *ep = ep_get(ep_id);
    uint16_t size = ep != NULL ? ep->msg_size : 0U;
    irq_spin_unlock(&ep_lock, crit);
    return size;
}

/* M2-Step3b: cap 路径 id ↔ 对象指针 转换 (header 在 offset 0)。 */
ep_id_t endpoint_id_from_obj(void *obj) {
    if (obj == NULL) return KERN_INVALID_ID;
    endpoint_t *ep = (endpoint_t *)obj;
    ep_id_t id = (ep_id_t)(ep - ep_pool);
    if (id < 0 || id >= KERN_MAX_ENDPOINTS) return KERN_INVALID_ID;
    return id;
}

void *endpoint_obj_for_cap(ep_id_t id) {
    if (id < 0 || id >= KERN_MAX_ENDPOINTS) return NULL;
    return (void *)&ep_pool[id];
}

kern_err_t endpoint_delete(ep_id_t ep_id) {
    uint32_t crit = irq_spin_lock(&ep_lock);

    endpoint_t *ep = ep_get(ep_id);
    if (ep == NULL) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_ERR_PARAM;
    }

    /* 唤醒所有等待的服务端 */
    tcb_t *tcb = ep->recv_waiters.head;
    while (tcb) {
        tcb_t *next = tcb->wait_next;
        if (tcb->id >= 0 && tcb->id < KERNEL_MAX_TASKS) {
            ep_syscall_recv_msg[tcb->id] = NULL;
            ep_syscall_recv_caps[tcb->id] = NULL;
            ep_syscall_recv_cap_count[tcb->id] = NULL;
        }
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
        if (tcb->id >= 0 && tcb->id < KERNEL_MAX_TASKS) {
            ep_client_msg[tcb->id] = NULL;
            ep_client_gen[tcb->id] = 0;
            ep_syscall_client_msg[tcb->id] = NULL;
        }
        tcb->wait_next = NULL;
        tcb->wait_prev = NULL;
        tcb->block_result = KERN_ERR_NOEXIST;
        sched_wakeup(tcb, KERN_ERR_NOEXIST);
        tcb = next;
    }

    memset(ep_server_sender[ep_id], 0, sizeof(ep_server_sender[ep_id]));
    memset(ep_server_gen[ep_id], 0, sizeof(ep_server_gen[ep_id]));
    memset(ep_server_dead[ep_id], 0, sizeof(ep_server_dead[ep_id]));
    ep_last_receiver[ep_id] = KERN_INVALID_ID;
    ep_last_receiver_gen[ep_id] = 0;
#if CAP_ENABLE
    for (task_id_t tid = 0; tid < KERNEL_MAX_TASKS; tid++) {
        endpoint_invalidate_reply_cap(ep_id, tid);
    }
#endif
    memset(ep_request_gen_buffers[ep_id], 0, sizeof(ep_request_gen_buffers[ep_id]));
    memset(ep_cap_xfer_buffers[ep_id], 0, sizeof(ep_cap_xfer_buffers[ep_id]));
    memset(ep_cap_count_buffers[ep_id], 0, sizeof(ep_cap_count_buffers[ep_id]));
#if CAP_ENABLE
    /* M2-Step1+3b: 撤销所有任务持有的指向此 endpoint 的 cap。Step3b 改真指针。 */
    (void)cap_revoke_object(ep, CAP_OBJ_ENDPOINT);
#endif
    /* M2-Step3b: bump generation 跨 memset 保留 */
    uint16_t next_gen = kobj_header_prepare_reuse(&ep->hdr);
    memset(ep, 0, sizeof(endpoint_t));
    ep->hdr.obj_type   = CAP_OBJ_ENDPOINT;
    ep->hdr.generation = next_gen;
    free_ep_id(ep_id);

    irq_spin_unlock(&ep_lock, crit);
    return KERN_OK;
}

int endpoint_exists(ep_id_t ep_id) {
    uint32_t crit = irq_spin_lock(&ep_lock);
    int exists = (ep_get(ep_id) != NULL) ? 1 : 0;
    irq_spin_unlock(&ep_lock, crit);
    return exists;
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
#if CAP_ENABLE
    endpoint_invalidate_sender_reply_caps((ep_id_t)(ep - ep_pool), tcb);
    if (tcb->id >= 0 && tcb->id < KERNEL_MAX_TASKS) {
        endpoint_invalidate_reply_cap((ep_id_t)(ep - ep_pool), tcb->id);
    }
#endif

    for (uint16_t i = 0; i < ep->max_pending; i++) {
        if (ep->sender_buf[i] == tcb) {
            ep->sender_buf[i] = NULL;
            ep_request_gen_buffers[(ep_id_t)(ep - ep_pool)][i] = 0;
        }
    }

    if (tcb->id >= 0 && tcb->id < KERNEL_MAX_TASKS) {
        ep_client_msg[tcb->id] = NULL;
        ep_client_gen[tcb->id] = 0;
        ep_syscall_client_msg[tcb->id] = NULL;
        ep_syscall_recv_msg[tcb->id] = NULL;
        ep_syscall_recv_caps[tcb->id] = NULL;
        ep_syscall_recv_cap_count[tcb->id] = NULL;
        ep_id_t ep_id = (ep_id_t)(ep - ep_pool);
        if (ep_last_receiver[ep_id] == tcb->id) {
            ep_last_receiver[ep_id] = KERN_INVALID_ID;
            ep_last_receiver_gen[ep_id] = 0;
        }
    }
}

static int endpoint_deliver_to_syscall_recv(ep_id_t ep_id,
                                            endpoint_t *ep,
                                            tcb_t *server,
                                            tcb_t *sender,
                                            const void *msg,
                                            const ipc_cap_xfer_t *caps,
                                            uint8_t cap_count,
                                            uint32_t request_gen) {
    if (ep == NULL || server == NULL || sender == NULL || msg == NULL) {
        return 0;
    }
    if (server->id < 0 || server->id >= KERNEL_MAX_TASKS) {
        return 0;
    }
    if (server->syscall_blocked == 0 || ep_syscall_recv_msg[server->id] == NULL) {
        return 0;
    }
    if (cap_count > 0 &&
        (ep_syscall_recv_caps[server->id] == NULL ||
         ep_syscall_recv_cap_count[server->id] == NULL)) {
        return 0;
    }

    memcpy(ep_syscall_recv_msg[server->id], msg, ep->msg_size);
    if (cap_count > 0) {
        kern_err_t err = ipc_transfer_caps(sender,
                                           server,
                                           caps,
                                           cap_count,
                                           ep_syscall_recv_caps[server->id]);
        if (err != KERN_OK) {
            sender->block_result = err;
            return 0;
        }
        *ep_syscall_recv_cap_count[server->id] = cap_count;
    } else if (ep_syscall_recv_cap_count[server->id] != NULL) {
        *ep_syscall_recv_cap_count[server->id] = 0;
    }

    ep_syscall_recv_msg[server->id] = NULL;
    ep_syscall_recv_caps[server->id] = NULL;
    ep_syscall_recv_cap_count[server->id] = NULL;
    ep_server_dead[ep_id][server->id] = 0;
    ep_server_sender[ep_id][server->id] = sender;
    ep_server_gen[ep_id][server->id] = request_gen;
    ep_last_receiver[ep_id] = server->id;
    ep_last_receiver_gen[ep_id] = request_gen;
#if CAP_ENABLE
    endpoint_bind_reply_cap(ep_id, server, sender, request_gen);
#endif

    wait_queue_remove_safe(&ep->recv_waiters, server);
    sched_wakeup(server, KERN_OK);
    return 1;
}

static kern_err_t endpoint_send_common(ep_id_t ep_id,
                                       void *msg,
                                       const ipc_cap_xfer_t *caps,
                                       uint8_t cap_count,
                                       uint32_t timeout) {
    if (hal_irq_get_active() >= 0) return KERN_ERR_ISR;
    if (cap_count > IPC_CAPS_MAX) return KERN_ERR_PARAM;
    if (cap_count > 0 && caps == NULL) return KERN_ERR_PARAM;

    uint32_t crit = irq_spin_lock(&ep_lock);

    endpoint_t *ep = ep_get(ep_id);
    if (ep == NULL) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();
    trace_record(TRACE_IPC_SEND, (uint8_t)current->id, (uint16_t)ep_id);

    /*
     * Sleepable syscall recv has no C stack to resume inside endpoint_recv().
     * If such a server is waiting, deliver directly to its validated user
     * buffer and bind reply authority before blocking the client for reply.
     */
    if (ep->recv_waiters.count > 0) {
        tcb_t *server = wait_queue_get_highest(&ep->recv_waiters);
        uint32_t request_gen = ep->next_request_gen++;
        if (ep->next_request_gen == 0) {
            ep->next_request_gen = 1;
        }

        if (endpoint_deliver_to_syscall_recv(ep_id, ep, server, current,
                                             msg, caps, cap_count, request_gen)) {
            ep_client_msg[current->id] = msg;
            ep_client_gen[current->id] = request_gen;

            wait_queue_add(&ep->reply_waiters, current);
            {
                extern void sched_remove_ready(tcb_t *tcb);
                sched_remove_ready(current);
            }
            current->state = TASK_STATE_BLOCKED;
            current->block_reason = BLOCK_REASON_EP_SEND;
            current->block_obj = ep;
            current->block_result = KERN_OK;
            if (timeout != KERN_WAIT_FOREVER) {
                extern uint32_t sched_get_tick_count(void);
                current->wake_tick = sched_get_tick_count() + timeout;
            } else {
                current->wake_tick = 0;
            }

            irq_spin_unlock(&ep_lock, crit);
            hal_trigger_pendsv();

            while (current->state == TASK_STATE_BLOCKED) {
                __asm volatile("wfi");
                __asm volatile("dmb");
            }

            kern_err_t result = current->block_result;
            if (result != KERN_OK) {
                crit = irq_spin_lock(&ep_lock);
                if (current->block_obj == ep) {
                    endpoint_cancel_sender(ep_id, ep, current);
                    current->block_obj = NULL;
                }
                irq_spin_unlock(&ep_lock, crit);
            }

            return result;
        }
    }

    /* 如果缓冲区已满，等待服务端腾出空间 */
    if (ep->count >= ep->max_pending) {
        if (timeout == 0) {
            irq_spin_unlock(&ep_lock, crit);
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
        if (timeout != KERN_WAIT_FOREVER) {
            extern uint32_t sched_get_tick_count(void);
            current->wake_tick = sched_get_tick_count() + timeout;
        } else {
            current->wake_tick = 0;
        }

        irq_spin_unlock(&ep_lock, crit);
        hal_trigger_pendsv();

        while (current->state == TASK_STATE_BLOCKED) {
            __asm volatile("wfi");
            __asm volatile("dmb");
        }

        if (current->block_result != KERN_OK) {
            crit = irq_spin_lock(&ep_lock);
            if (current->block_obj == ep) {
                wait_queue_remove_safe(&ep->send_waiters, current);
                current->block_obj = NULL;
            }
            irq_spin_unlock(&ep_lock, crit);
            return current->block_result;
        }

        /* 被唤醒后重新检查 */
        crit = irq_spin_lock(&ep_lock);
        ep = ep_get(ep_id);
        if (ep == NULL) {
            irq_spin_unlock(&ep_lock, crit);
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
    if (timeout != KERN_WAIT_FOREVER) {
        extern uint32_t sched_get_tick_count(void);
        current->wake_tick = sched_get_tick_count() + timeout;
    } else {
        current->wake_tick = 0;
    }

    irq_spin_unlock(&ep_lock, crit);
    hal_trigger_pendsv();

    while (current->state == TASK_STATE_BLOCKED) {
        __asm volatile("wfi");
        __asm volatile("dmb");
    }

    kern_err_t result = current->block_result;
    if (result != KERN_OK) {
        crit = irq_spin_lock(&ep_lock);
        if (current->block_obj == ep) {
            endpoint_cancel_sender(ep_id, ep, current);
            current->block_obj = NULL;
        }
        irq_spin_unlock(&ep_lock, crit);
    }

    return result;
}

kern_err_t endpoint_send(ep_id_t ep_id, void *msg, uint32_t timeout) {
    return endpoint_send_common(ep_id, msg, NULL, 0, timeout);
}

kern_err_t endpoint_notify(ep_id_t ep_id, const void *msg) {
    if (hal_irq_get_active() >= 0) return KERN_ERR_ISR;
    if (msg == NULL) return KERN_ERR_PARAM;

    uint32_t crit = irq_spin_lock(&ep_lock);

    endpoint_t *ep = ep_get(ep_id);
    if (ep == NULL) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_ERR_PARAM;
    }

#if SYSCALL_ENABLE
    if (ep->recv_waiters.count > 0) {
        tcb_t *server = wait_queue_get_highest(&ep->recv_waiters);
        if (server != NULL &&
            server->syscall_blocked &&
            server->id >= 0 && server->id < KERNEL_MAX_TASKS &&
            ep_syscall_recv_msg[server->id] != NULL) {
            memcpy(ep_syscall_recv_msg[server->id], msg, ep->msg_size);
            if (ep_syscall_recv_cap_count[server->id] != NULL) {
                *ep_syscall_recv_cap_count[server->id] = 0;
            }
            ep_syscall_recv_msg[server->id] = NULL;
            ep_syscall_recv_caps[server->id] = NULL;
            ep_syscall_recv_cap_count[server->id] = NULL;
            ep_server_dead[ep_id][server->id] = 0;
            ep_server_sender[ep_id][server->id] = NULL;
            ep_server_gen[ep_id][server->id] = 0;
#if CAP_ENABLE
            endpoint_invalidate_reply_cap(ep_id, server->id);
#endif
            wait_queue_remove_safe(&ep->recv_waiters, server);
            sched_wakeup(server, KERN_OK);
            irq_spin_unlock(&ep_lock, crit);
            return KERN_OK;
        }
    }
#endif

    if (ep->count >= ep->max_pending) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_ERR_BUSY;
    }

    uint16_t slot = ep->head;
    uint8_t *dst = ep->msg_buf + (slot * ep->msg_size);
    memcpy(dst, msg, ep->msg_size);
    ep->sender_buf[slot] = NULL;
    ep_request_gen_buffers[ep_id][slot] = 0;
    ep_cap_count_buffers[ep_id][slot] = 0;
    ep->head = (ep->head + 1) % ep->max_pending;
    ep->count++;
    ep->pending_count++;

    if (ep->recv_waiters.count > 0) {
        tcb_t *server = wait_queue_get_highest(&ep->recv_waiters);
        if (server != NULL) {
            wait_queue_remove(&ep->recv_waiters, server);
            server->block_result = KERN_OK;
            sched_wakeup(server, KERN_OK);
        }
    }

    irq_spin_unlock(&ep_lock, crit);
    return KERN_OK;
}

static kern_err_t endpoint_send_syscall_common(ep_id_t ep_id,
                                               const void *msg,
                                               void *user_reply_msg,
                                               const ipc_cap_xfer_t *caps,
                                               uint8_t cap_count,
                                               uint32_t timeout) {
    if (hal_irq_get_active() >= 0) return KERN_ERR_ISR;
    if (msg == NULL || user_reply_msg == NULL) return KERN_ERR_PARAM;
    if (cap_count > IPC_CAPS_MAX) return KERN_ERR_PARAM;
    if (cap_count > 0 && caps == NULL) return KERN_ERR_PARAM;

    uint32_t crit = irq_spin_lock(&ep_lock);

    endpoint_t *ep = ep_get(ep_id);
    if (ep == NULL) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();
    if (current == NULL || current->id < 0 || current->id >= KERNEL_MAX_TASKS) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_ERR_STATE;
    }
    trace_record(TRACE_IPC_SEND, (uint8_t)current->id, (uint16_t)ep_id);

    if (timeout == 0) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_ERR_TIMEOUT;
    }

    if (ep->count >= ep->max_pending) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_ERR_BUSY;
    }

    uint32_t request_gen = ep->next_request_gen++;
    if (ep->next_request_gen == 0) {
        ep->next_request_gen = 1;
    }

    ep_client_msg[current->id] = user_reply_msg;
    ep_syscall_client_msg[current->id] = user_reply_msg;
    ep_client_gen[current->id] = request_gen;

    wait_queue_add(&ep->reply_waiters, current);
    {
        extern void sched_remove_ready(tcb_t *tcb);
        sched_remove_ready(current);
    }

    current->syscall_blocked = 1;
    current->state = TASK_STATE_BLOCKED;
    current->block_reason = BLOCK_REASON_EP_SEND;
    current->block_obj = ep;
    current->block_result = KERN_OK;
    if (timeout != KERN_WAIT_FOREVER) {
        extern uint32_t sched_get_tick_count(void);
        current->wake_tick = sched_get_tick_count() + timeout;
    } else {
        current->wake_tick = 0;
    }

    if (ep->recv_waiters.count > 0) {
        tcb_t *server = wait_queue_get_highest(&ep->recv_waiters);
        if (!endpoint_deliver_to_syscall_recv(ep_id, ep, server, current,
                                             msg, caps, cap_count, request_gen)) {
            if (current->block_result != KERN_OK) {
                kern_err_t err = current->block_result;
                wait_queue_remove_safe(&ep->reply_waiters, current);
                ep_client_msg[current->id] = NULL;
                ep_syscall_client_msg[current->id] = NULL;
                ep_client_gen[current->id] = 0;
                current->syscall_blocked = 0;
                current->state = TASK_STATE_RUNNING;
                current->block_reason = BLOCK_REASON_NONE;
                current->block_obj = NULL;
                current->wake_tick = 0;
                current->block_result = KERN_OK;
                irq_spin_unlock(&ep_lock, crit);
                return err;
            }
            uint16_t slot = ep->head;
            uint8_t *dst = ep->msg_buf + (slot * ep->msg_size);
            memcpy(dst, msg, ep->msg_size);
            ep->sender_buf[slot] = current;
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

            if (server) {
                wait_queue_remove(&ep->recv_waiters, server);
                server->block_result = KERN_OK;
                sched_wakeup(server, KERN_OK);
            }
        }
    } else {
        uint16_t slot = ep->head;
        uint8_t *dst = ep->msg_buf + (slot * ep->msg_size);
        memcpy(dst, msg, ep->msg_size);
        ep->sender_buf[slot] = current;
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

        if (ep->recv_waiters.count > 0) {
            tcb_t *server = wait_queue_get_highest(&ep->recv_waiters);
            if (server) {
                wait_queue_remove(&ep->recv_waiters, server);
                server->block_result = KERN_OK;
                sched_wakeup(server, KERN_OK);
            }
        }
    }

    irq_spin_unlock(&ep_lock, crit);
    return KERN_SYSCALL_BLOCKED;
}

kern_err_t endpoint_send_syscall(ep_id_t ep_id,
                                 const void *msg,
                                 void *user_reply_msg,
                                 uint32_t timeout) {
    return endpoint_send_syscall_common(ep_id, msg, user_reply_msg,
                                        NULL, 0, timeout);
}

kern_err_t endpoint_send_caps_syscall(ep_id_t ep_id,
                                      const void *msg,
                                      void *user_reply_msg,
                                      const ipc_cap_xfer_t *caps,
                                      uint8_t cap_count,
                                      uint32_t timeout) {
    return endpoint_send_syscall_common(ep_id, msg, user_reply_msg,
                                        caps, cap_count, timeout);
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

    uint32_t crit = irq_spin_lock(&ep_lock);

    endpoint_t *ep = ep_get(ep_id);
    if (ep == NULL) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();
    trace_record(TRACE_IPC_RECV, (uint8_t)current->id, (uint16_t)ep_id);

    /* 如果没有待处理消息，阻塞等待 */
    if (ep->count == 0) {
        if (timeout == 0) {
            irq_spin_unlock(&ep_lock, crit);
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
        if (timeout != KERN_WAIT_FOREVER) {
            extern uint32_t sched_get_tick_count(void);
            current->wake_tick = sched_get_tick_count() + timeout;
        } else {
            current->wake_tick = 0;
        }

        irq_spin_unlock(&ep_lock, crit);
        hal_trigger_pendsv();

        while (current->state == TASK_STATE_BLOCKED) {
            __asm volatile("wfi");
            __asm volatile("dmb");
        }

        if (current->block_result != KERN_OK) {
            crit = irq_spin_lock(&ep_lock);
            if (current->block_obj == ep) {
                wait_queue_remove(&ep->recv_waiters, current);
                current->block_obj = NULL;
            }
            irq_spin_unlock(&ep_lock, crit);
            return current->block_result;
        }

        crit = irq_spin_lock(&ep_lock);
        ep = ep_get(ep_id);
        if (ep == NULL) {
            irq_spin_unlock(&ep_lock, crit);
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
            irq_spin_unlock(&ep_lock, crit);
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
            irq_spin_unlock(&ep_lock, crit);
            return xfer_err;
        }
        *out_cap_count = cap_count;
    } else if (out_cap_count != NULL) {
        *out_cap_count = 0;
    }

    if (current->id >= 0 && current->id < KERNEL_MAX_TASKS) {
        ep_server_sender[ep_id][current->id] = sender;
        ep_server_gen[ep_id][current->id] = request_gen;
        ep_last_receiver[ep_id] = current->id;
        ep_last_receiver_gen[ep_id] = request_gen;
#if CAP_ENABLE
        endpoint_bind_reply_cap(ep_id, current, sender, request_gen);
#endif
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

    irq_spin_unlock(&ep_lock, crit);
    return KERN_OK;
}

kern_err_t endpoint_recv(ep_id_t ep_id, void *msg, uint32_t timeout) {
    return endpoint_recv_common(ep_id, msg, NULL, NULL, timeout);
}

kern_err_t endpoint_recv_syscall(ep_id_t ep_id, void *user_msg, uint32_t timeout) {
    if (hal_irq_get_active() >= 0) return KERN_ERR_ISR;
    if (user_msg == NULL) return KERN_ERR_PARAM;

    uint32_t crit = irq_spin_lock(&ep_lock);

    endpoint_t *ep = ep_get(ep_id);
    if (ep == NULL) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();
    if (current == NULL || current->id < 0 || current->id >= KERNEL_MAX_TASKS) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_ERR_STATE;
    }
    trace_record(TRACE_IPC_RECV, (uint8_t)current->id, (uint16_t)ep_id);

    if (ep->count > 0) {
        uint16_t slot = ep->tail;
        uint8_t cap_count = ep_cap_count_buffers[ep_id][slot];
        if (cap_count > 0) {
            tcb_t *sender = ep->sender_buf[slot];
            if (sender != NULL) {
                wait_queue_remove_safe(&ep->reply_waiters, sender);
                sender->block_result = KERN_ERR_RESOURCE;
                sched_wakeup(sender, KERN_ERR_RESOURCE);
            }
            ep_cap_count_buffers[ep_id][slot] = 0;
            ep_request_gen_buffers[ep_id][slot] = 0;
            ep->sender_buf[slot] = NULL;
            ep->tail = (ep->tail + 1) % ep->max_pending;
            ep->count--;
            ep->pending_count--;
            irq_spin_unlock(&ep_lock, crit);
            return KERN_ERR_RESOURCE;
        }

        uint8_t *src = ep->msg_buf + (slot * ep->msg_size);
        memcpy(user_msg, src, ep->msg_size);

        tcb_t *sender = ep->sender_buf[slot];
        uint32_t request_gen = ep_request_gen_buffers[ep_id][slot];
        ep_server_dead[ep_id][current->id] = 0;
        ep_server_sender[ep_id][current->id] = sender;
        ep_server_gen[ep_id][current->id] = request_gen;
        ep_last_receiver[ep_id] = current->id;
        ep_last_receiver_gen[ep_id] = request_gen;
#if CAP_ENABLE
        endpoint_bind_reply_cap(ep_id, current, sender, request_gen);
#endif

        ep_cap_count_buffers[ep_id][slot] = 0;
        ep_request_gen_buffers[ep_id][slot] = 0;
        ep->sender_buf[slot] = NULL;
        ep->tail = (ep->tail + 1) % ep->max_pending;
        ep->count--;
        ep->pending_count--;

        tcb_t *waiter = wait_queue_get_highest(&ep->send_waiters);
        if (waiter) {
            wait_queue_remove(&ep->send_waiters, waiter);
            waiter->block_result = KERN_OK;
            sched_wakeup(waiter, KERN_OK);
        }

        irq_spin_unlock(&ep_lock, crit);
        return KERN_OK;
    }

    if (timeout == 0) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_ERR_TIMEOUT;
    }

    ep_syscall_recv_msg[current->id] = user_msg;
    current->syscall_blocked = 1;
    current->block_reason = BLOCK_REASON_EP_RECV;
    current->block_obj = ep;
    wait_queue_add(&ep->recv_waiters, current);

    {
        extern void sched_remove_ready(tcb_t *tcb);
        sched_remove_ready(current);
    }

    current->state = TASK_STATE_BLOCKED;
    current->block_result = KERN_OK;
    if (timeout != KERN_WAIT_FOREVER) {
        extern uint32_t sched_get_tick_count(void);
        current->wake_tick = sched_get_tick_count() + timeout;
    } else {
        current->wake_tick = 0;
    }

    irq_spin_unlock(&ep_lock, crit);
    return KERN_SYSCALL_BLOCKED;
}

kern_err_t endpoint_recv_caps(ep_id_t ep_id,
                              void *msg,
                              cap_id_t *out_caps,
                              uint8_t *out_cap_count,
                              uint32_t timeout) {
    return endpoint_recv_common(ep_id, msg, out_caps, out_cap_count, timeout);
}

kern_err_t endpoint_recv_caps_syscall(ep_id_t ep_id,
                                      void *user_msg,
                                      cap_id_t *out_caps,
                                      uint8_t *out_cap_count,
                                      uint32_t timeout) {
    if (hal_irq_get_active() >= 0) return KERN_ERR_ISR;
    if (user_msg == NULL || out_caps == NULL || out_cap_count == NULL) {
        return KERN_ERR_PARAM;
    }

    uint32_t crit = irq_spin_lock(&ep_lock);

    endpoint_t *ep = ep_get(ep_id);
    if (ep == NULL) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_ERR_PARAM;
    }

    tcb_t *current = sched_get_current();
    if (current == NULL || current->id < 0 || current->id >= KERNEL_MAX_TASKS) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_ERR_STATE;
    }

    if (ep->count > 0) {
        irq_spin_unlock(&ep_lock, crit);
        return endpoint_recv_common(ep_id, user_msg, out_caps, out_cap_count, 0);
    }

    if (timeout == 0) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_ERR_TIMEOUT;
    }

    ep_syscall_recv_msg[current->id] = user_msg;
    ep_syscall_recv_caps[current->id] = out_caps;
    ep_syscall_recv_cap_count[current->id] = out_cap_count;
    current->syscall_blocked = 1;
    current->block_reason = BLOCK_REASON_EP_RECV;
    current->block_obj = ep;
    wait_queue_add(&ep->recv_waiters, current);

    {
        extern void sched_remove_ready(tcb_t *tcb);
        sched_remove_ready(current);
    }

    current->state = TASK_STATE_BLOCKED;
    current->block_result = KERN_OK;
    if (timeout != KERN_WAIT_FOREVER) {
        extern uint32_t sched_get_tick_count(void);
        current->wake_tick = sched_get_tick_count() + timeout;
    } else {
        current->wake_tick = 0;
    }

    irq_spin_unlock(&ep_lock, crit);
    return KERN_SYSCALL_BLOCKED;
}

static kern_err_t endpoint_reply_bound(ep_id_t ep_id, tcb_t *server,
                                       tcb_t *sender, uint32_t request_gen,
                                       const void *msg) {
    uint32_t crit = irq_spin_lock(&ep_lock);

    endpoint_t *ep = ep_get(ep_id);
    if (ep == NULL) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_ERR_PARAM;
    }

    if (sender == NULL) {
        if (server != NULL && server->id >= 0 && server->id < KERNEL_MAX_TASKS &&
            ep_server_dead[ep_id][server->id] != 0) {
            ep_server_dead[ep_id][server->id] = 0;
#if CAP_ENABLE
            endpoint_invalidate_reply_cap(ep_id, server->id);
#endif
            irq_spin_unlock(&ep_lock, crit);
            return KERN_ERR_NOEXIST;
        }
        irq_spin_unlock(&ep_lock, crit);
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
#if CAP_ENABLE
            endpoint_invalidate_reply_cap(ep_id, server->id);
#endif
        }
        irq_spin_unlock(&ep_lock, crit);
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
        if (ep_last_receiver[ep_id] == server->id &&
            ep_last_receiver_gen[ep_id] == request_gen) {
            ep_last_receiver[ep_id] = KERN_INVALID_ID;
            ep_last_receiver_gen[ep_id] = 0;
        }
#if CAP_ENABLE
        endpoint_invalidate_reply_cap(ep_id, server->id);
#endif
    }
    ep_client_msg[sender->id] = NULL;
    ep_client_gen[sender->id] = 0;
    ep_syscall_client_msg[sender->id] = NULL;

    /* 从 reply_waiters 移除并唤醒客户端 */
    wait_queue_remove_safe(&ep->reply_waiters, sender);
    sender->block_result = KERN_OK;
    sched_wakeup(sender, KERN_OK);

    irq_spin_unlock(&ep_lock, crit);
    return KERN_OK;
}

kern_err_t endpoint_reply(ep_id_t ep_id, const void *msg) {
    tcb_t *server = sched_get_current();
    tcb_t *sender = NULL;
    uint32_t request_gen = 0;

    if (server != NULL && server->id >= 0 && server->id < KERNEL_MAX_TASKS &&
        ep_id >= 0 && ep_id < KERN_MAX_ENDPOINTS) {
        sender = ep_server_sender[ep_id][server->id];
        request_gen = ep_server_gen[ep_id][server->id];

        if (sender == NULL || request_gen == 0) {
            uint32_t crit = irq_spin_lock(&ep_lock);
            endpoint_t *ep = ep_get(ep_id);
            if (ep != NULL &&
                ep_last_receiver[ep_id] == server->id &&
                ep_last_receiver_gen[ep_id] != 0) {
                tcb_t *waiter = wait_queue_get_highest(&ep->reply_waiters);
                if (waiter != NULL &&
                    waiter->id >= 0 && waiter->id < KERNEL_MAX_TASKS &&
                    waiter->block_obj == ep &&
                    waiter->block_reason == BLOCK_REASON_EP_SEND) {
                    sender = waiter;
                    request_gen = ep_last_receiver_gen[ep_id];
                    ep_server_sender[ep_id][server->id] = sender;
                    ep_server_gen[ep_id][server->id] = request_gen;
                    ep_server_dead[ep_id][server->id] = 0;
#if CAP_ENABLE
                    endpoint_bind_reply_cap(ep_id, server, sender, request_gen);
#endif
                }
            }
            irq_spin_unlock(&ep_lock, crit);
        }
    }

    return endpoint_reply_bound(ep_id, server, sender, request_gen, msg);
}

#if CAP_ENABLE
cap_id_t endpoint_take_reply_cap(ep_id_t ep_id) {
    tcb_t *server = sched_get_current();
    if (server == NULL || server->id < 0 || server->id >= KERNEL_MAX_TASKS ||
        ep_id < 0 || ep_id >= KERN_MAX_ENDPOINTS) {
        return KERN_INVALID_ID;
    }

    uint32_t crit = irq_spin_lock(&ep_lock);
    endpoint_t *ep = ep_get(ep_id);
    if (ep == NULL) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_INVALID_ID;
    }

    endpoint_reply_t *reply = &ep_reply_objects[ep_id][server->id];
    if (!reply->active && !reply->used) {
        tcb_t *sender = ep_server_sender[ep_id][server->id];
        uint32_t request_gen = ep_server_gen[ep_id][server->id];
        if (sender == NULL) {
            sender = wait_queue_get_highest(&ep->reply_waiters);
            if (sender != NULL &&
                sender->id >= 0 && sender->id < KERNEL_MAX_TASKS &&
                sender->block_obj == ep &&
                sender->block_reason == BLOCK_REASON_EP_SEND) {
                request_gen = ep_client_gen[sender->id];
                ep_server_sender[ep_id][server->id] = sender;
                ep_server_gen[ep_id][server->id] = request_gen;
                ep_server_dead[ep_id][server->id] = 0;
            }
        }
        if (sender != NULL && request_gen != 0) {
            endpoint_bind_reply_cap(ep_id, server, sender, request_gen);
        }
    }

    if (!reply->active || reply->used) {
        if (reply->bind_error != KERN_OK) {
            kern_err_t err = reply->bind_error;
            irq_spin_unlock(&ep_lock, crit);
            return err;
        }
        irq_spin_unlock(&ep_lock, crit);
        return KERN_INVALID_ID;
    }
    cap_id_t cap = ep_server_reply_cap[ep_id][server->id];
    if (cap == KERN_INVALID_ID) {
        irq_spin_unlock(&ep_lock, crit);
        return KERN_INVALID_ID;
    }
    irq_spin_unlock(&ep_lock, crit);
    return cap;
}

kern_err_t endpoint_reply_cap(void *reply_obj, const void *msg) {
    endpoint_reply_t *reply = (endpoint_reply_t *)reply_obj;
    tcb_t *server = sched_get_current();

    if (reply == NULL || !reply->active || reply->used) {
        return KERN_ERR_STATE;
    }
    if (server == NULL || server->id != reply->server_id) {
        return KERN_ERR_CAP;
    }

    return endpoint_reply_bound(reply->ep_id, server, reply->sender,
                                reply->request_gen, msg);
}
#endif

/* 返回当前任务在指定 endpoint 上最近一次 recv 的 sender task id。
 * fs_server 用它记录"谁打开了这个 fd",客户端死亡时精确清理。 */
task_id_t endpoint_last_sender(ep_id_t ep_id) {
    tcb_t *current = sched_get_current();
    if (current == NULL || ep_id < 0 || ep_id >= KERN_MAX_ENDPOINTS) {
        return KERN_INVALID_ID;
    }
    tcb_t *sender = ep_server_sender[ep_id][current->id];
    if (sender == NULL) {
        return KERN_INVALID_ID;
    }
    return sender->id;
}

#endif /* IPC_ENDPOINT */
