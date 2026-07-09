/**
 * @file sync_proto.h
 * @brief Phase H1 — 用户态同步原语协议 (基于 endpoint)
 *
 * sync_server 是用户态 lock/sem 服务,基于 endpoint IPC 实现:
 *   client → SYNC_OP_LOCK → server (如果 free,立即 reply;如果 locked,
 *           持有 request 不 reply,client 阻塞)
 *   client → SYNC_OP_UNLOCK → server (reply + 唤醒一个等待者)
 *
 * 这取代内核的 sem/mutex syscall (真微内核的同步原语应在用户态)。
 * 内核 sem/mutex 暂保留 (内核内部用,如 bh_sem/timer)。
 */

#ifndef SYNC_PROTO_H
#define SYNC_PROTO_H

#include "kernel_types.h"
#include <stdint.h>

#define SYNC_MAGIC     0x53594E43U   /* "SYNC" */
#define SYNC_MAX_LOCKS 16U

#define SYNC_OP_PING   1U
#define SYNC_OP_LOCK   2U            /* 请求锁 (阻塞直到获得) */
#define SYNC_OP_UNLOCK 3U            /* 释放锁 */
#define SYNC_OP_TRYLOCK 4U           /* 非阻塞尝试锁 */

typedef struct {
    uint32_t magic;
    uint16_t opcode;
    uint16_t reserved;
    uint32_t lock_id;                /* 锁编号 (0..SYNC_MAX_LOCKS-1) */
    int32_t  status;                 /* reply: KERN_OK 或错误码 */
    uint8_t  padding[KERN_EP_MSG_SIZE - 16];  /* 填充到 endpoint msg 大小 */
} sync_msg_t;

/* sync_server 服务循环 (用户态任务) */
int sync_server_run(int ep_cap, uint32_t max_requests);

/* 客户端 API */
int sync_ping(int ep_cap, uint32_t timeout);
int sync_lock(int ep_cap, uint32_t lock_id, uint32_t timeout);
int sync_unlock(int ep_cap, uint32_t lock_id, uint32_t timeout);
int sync_trylock(int ep_cap, uint32_t lock_id, uint32_t timeout);

static inline int sync_opcode_valid(uint16_t opcode) {
    return opcode >= SYNC_OP_PING && opcode <= SYNC_OP_TRYLOCK;
}

#endif /* SYNC_PROTO_H */
