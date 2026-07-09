/**
 * @file sync_server.c
 * @brief Phase H1 — 用户态同步原语服务 (基于 endpoint)
 *
 * sync_server 提供 lock/unlock/trylock,基于 endpoint IPC。
 * 取代内核的 sem/mutex syscall (真微内核同步原语在用户态)。
 *
 * 当前实现:trylock (非阻塞) + unlock。阻塞 lock 留后续
 * (需要 endpoint 的 take_reply_cap 延迟 reply 机制)。
 *
 * 客户端用 sync_lock 实现阻塞:trylock 失败时 client 自己重试
 * (带 sys_task_delay 退避)。
 */

#include "sync_proto.h"
#include "user_api.h"

#if CAP_ENABLE

/*============================================================================
 * 锁状态
 *============================================================================*/

typedef struct {
    uint8_t in_use;       /* 锁是否被定义 */
    uint8_t locked;       /* 是否被持有 */
} sync_lock_t;

static void sync_zero(void *p, uint32_t n) {
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) b[i] = 0;
}

/*============================================================================
 * 服务循环
 *============================================================================*/

int sync_server_run(int ep_cap, uint32_t max_requests) {
    /* 锁状态放栈上 (user 任务不能访问 .bss 全局,MPU 禁止) */
    sync_lock_t locks[SYNC_MAX_LOCKS];
    sync_msg_t msg;
    int err = KERN_OK;

    if (ep_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    sync_zero(locks, sizeof(locks));

    for (uint32_t round = 0;
         (max_requests == 0U || round < max_requests) && err == KERN_OK;
         round++) {
        sync_zero(&msg, sizeof(msg));
        err = sys_ep_recv(ep_cap, &msg, 1000);
        if (err == KERN_ERR_TIMEOUT && max_requests == 0U) {
            err = KERN_OK;
            continue;
        }
        if (err != KERN_OK) {
            break;
        }

        if (msg.magic != SYNC_MAGIC || !sync_opcode_valid(msg.opcode)) {
            msg.status = KERN_ERR_PARAM;
            (void)sys_ep_reply(ep_cap, &msg);
            continue;
        }

        if (msg.opcode == SYNC_OP_PING) {
            msg.status = KERN_OK;
            (void)sys_ep_reply(ep_cap, &msg);
            continue;
        }

        /* 锁操作 */
        if (msg.lock_id >= SYNC_MAX_LOCKS) {
            msg.status = KERN_ERR_PARAM;
            (void)sys_ep_reply(ep_cap, &msg);
            continue;
        }

        sync_lock_t *lock = &locks[msg.lock_id];

        if (msg.opcode == SYNC_OP_LOCK || msg.opcode == SYNC_OP_TRYLOCK) {
            if (!lock->in_use) {
                lock->in_use = 1;
            }
            if (!lock->locked) {
                lock->locked = 1;
                msg.status = KERN_OK;  /* 获得锁 */
            } else {
                msg.status = KERN_ERR_BUSY;  /* 锁忙 */
            }
            (void)sys_ep_reply(ep_cap, &msg);

        } else if (msg.opcode == SYNC_OP_UNLOCK) {
            if (!lock->in_use) {
                msg.status = KERN_ERR_NOEXIST;
            } else {
                lock->locked = 0;
                msg.status = KERN_OK;
            }
            (void)sys_ep_reply(ep_cap, &msg);

        } else {
            msg.status = KERN_ERR_PARAM;
            (void)sys_ep_reply(ep_cap, &msg);
        }
    }

    return err;
}

/*============================================================================
 * 客户端 API
 *============================================================================*/

int sync_ping(int ep_cap, uint32_t timeout) {
    sync_msg_t msg;
    sync_zero(&msg, sizeof(msg));
    msg.magic = SYNC_MAGIC;
    msg.opcode = SYNC_OP_PING;
    int err = sys_ep_send(ep_cap, &msg, (int)timeout);
    if (err != KERN_OK) return err;
    return msg.status;
}

int sync_trylock(int ep_cap, uint32_t lock_id, uint32_t timeout) {
    sync_msg_t msg;
    sync_zero(&msg, sizeof(msg));
    msg.magic = SYNC_MAGIC;
    msg.opcode = SYNC_OP_TRYLOCK;
    msg.lock_id = lock_id;
    int err = sys_ep_send(ep_cap, &msg, (int)timeout);
    if (err != KERN_OK) return err;
    return msg.status;
}

int sync_lock(int ep_cap, uint32_t lock_id, uint32_t timeout) {
    /* 阻塞 lock:trylock + 重试退避。
     * 简化实现 (无延迟 reply),后续可改成 server 端阻塞。 */
    uint32_t elapsed = 0;
    while (elapsed < timeout) {
        int err = sync_trylock(ep_cap, lock_id, 200);
        if (err == KERN_OK) return KERN_OK;
        if (err != KERN_ERR_BUSY) return err;
        sys_task_delay(10);
        elapsed += 10;
    }
    return KERN_ERR_TIMEOUT;
}

int sync_unlock(int ep_cap, uint32_t lock_id, uint32_t timeout) {
    sync_msg_t msg;
    sync_zero(&msg, sizeof(msg));
    msg.magic = SYNC_MAGIC;
    msg.opcode = SYNC_OP_UNLOCK;
    msg.lock_id = lock_id;
    int err = sys_ep_send(ep_cap, &msg, (int)timeout);
    if (err != KERN_OK) return err;
    return msg.status;
}

#endif /* CAP_ENABLE */
