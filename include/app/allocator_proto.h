/**
 * @file allocator_proto.h
 * @brief User-space allocator-service IPC ABI
 *
 * allocator 是特权服务,作为用户态任务创建共享内存 (shm) 的唯一仲裁者。
 * 背景:sys_shm_create 拒绝普通 user 任务 (syscall.c 的有意设计),
 * 因此需要由 allocator 这个特权任务代为创建,再通过 endpoint IPC 把
 * shm cap (带 CAP_TRANSFER) 转交给请求方。
 *
 * 协议流程 (CREATE):
 *   client                                            allocator
 *     │ sys_ep_create inbox                              │
 *     │ xfers[0] = { inbox_cap, RW, COPY }              │
 *     │── sys_ep_send_caps(alloc_ep, msg, xfers, 1) ───→│
 *     │                                                  │ sys_shm_create(size, rights)
 *     │←──────────── sys_ep_reply(alloc_ep, OK) ─────────│
 *     │                                                  │ sys_ep_send_caps(inbox, msg,
 *     │←───── sys_ep_recv_caps(inbox, ..., &shm_cap) ────│              {shm_cap, RW|TRANSFER, COPY})
 *     │ sys_shm_map(shm_cap, RW) → 拿到指针              │
 *
 * 客户端示例见 src/tests/test_allocator.c。
 */

#ifndef ALLOCATOR_PROTO_H
#define ALLOCATOR_PROTO_H

#include "kernel_types.h"
#include <stdint.h>

#define ALLOC_MAGIC     0x414C4F43U   /* "ALOC" */
#define ALLOC_NAME_MAX  24U

#define ALLOC_OP_PING   1U
#define ALLOC_OP_CREATE 2U            /* 创建 shm,返回 shm cap */
#define ALLOC_OP_DESTROY 3U           /* 销毁 shm (目前用 sys_cap_revoke 替代) */

#define ALLOC_FLAG_NONE 0U

/* shm 创建请求的 rights 预设 */
#define ALLOC_RIGHTS_RW   0U          /* 默认 RW(allocator 内部映射成 CAP_READ|CAP_WRITE|TRANSFER) */

/**
 * allocator IPC 消息。
 * 复用 KERN_EP_MSG_SIZE 对齐 endpoint 消息缓冲。
 */
typedef struct {
    uint32_t magic;        /*  4 */
    uint16_t opcode;       /*  6 */
    uint16_t flags;        /*  8 */
    uint32_t seq;          /* 12 */
    int32_t  status;       /* 16 */
    int32_t  result;       /* 20 */
    uint32_t size;         /* 24 */
    uint32_t rights;       /* 28 */
    uint8_t  reserved[KERN_EP_MSG_SIZE - 28U];
} alloc_msg_t;

/**
 * allocator 服务循环。
 *
 * @param ep_cap   allocator 自己的 endpoint cap (READ|WRITE)
 * @param max_requests  0 = 无限循环;>0 = 处理该数量后退出 (测试用)
 * @return KERN_OK 或错误码
 *
 * 注意:allocator 必须由特权任务 (task_create, 非 task_create_user) 运行,
 * 因为它内部调 sys_shm_create,该 syscall 拒绝 user 任务。
 */
int allocator_service_run(int ep_cap, uint32_t max_requests);

/* ---------- 客户端 API ---------- */

int allocator_ping(int alloc_ep_cap, uint32_t timeout);

/**
 * 向 allocator 请求创建 shm。
 *
 * @param alloc_ep_cap  allocator endpoint cap
 * @param inbox_cap     客户端自己的 inbox endpoint cap (用于接收 shm cap)
 * @param size          shm 字节数 (>=32 且 32 对齐,见 kshm_is_mpu_compliant)
 * @param rights        权限预设 (ALLOC_RIGHTS_RW)
 * @param out_shm_cap   [out] 收到的 shm cap
 * @param timeout       超时 (ms)
 * @return KERN_OK 或错误码
 */
int allocator_create_shm(int alloc_ep_cap, int inbox_cap,
                         uint32_t size, uint32_t rights,
                         int *out_shm_cap, uint32_t timeout);

static inline int alloc_opcode_valid(uint16_t opcode) {
    return opcode >= ALLOC_OP_PING && opcode <= ALLOC_OP_DESTROY;
}

static inline void alloc_msg_init(alloc_msg_t *msg, uint16_t opcode,
                                  uint32_t seq) {
    if (msg == NULL) {
        return;
    }
    /* 只设协议字段,不清 reserved (调用方负责缓冲清零,见 alloc_msg_clear) */
    msg->magic = ALLOC_MAGIC;
    msg->opcode = opcode;
    msg->flags = ALLOC_FLAG_NONE;
    msg->seq = seq;
    msg->status = 0;
    msg->result = 0;
    msg->size = 0;
    msg->rights = 0;
}

/* 彻底清零整个消息缓冲 (在 recv 前调用) */
static inline void alloc_msg_clear(alloc_msg_t *msg) {
    if (msg == NULL) {
        return;
    }
    msg->magic = 0;
    msg->opcode = 0;
    msg->flags = 0;
    msg->seq = 0;
    msg->status = 0;
    msg->result = 0;
    msg->size = 0;
    msg->rights = 0;
}

#endif /* ALLOCATOR_PROTO_H */
