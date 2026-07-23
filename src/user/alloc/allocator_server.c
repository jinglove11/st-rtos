/**
 * @file allocator_server.c
 * @brief User-space allocator service — privileged shm broker
 *
 * allocator 是特权任务,唯一职责:替普通 user 任务创建共享内存 (shm)。
 *
 * 为什么需要它:sys_shm_create (syscall.c) 拒绝 user 任务 (有意设计,
 * 见 SHM_MAP_DESIGN.md)。fs_server 等 user 服务需要 shm 传大数据,
 * 必须经过 allocator 这个特权仲裁者。
 *
 * 工作模式完全复刻 nameserver 的 LOOKUP:
 *   1. client 随请求发来自己的 inbox cap
 *   2. allocator reply OK
 *   3. allocator 把新建的 shm cap 推到 client 的 inbox
 *   4. client 在 inbox 上 recv_caps 拿到 shm cap
 *
 * 必须用 task_create (特权) 而非 task_create_user 拉起。
 */

#include "allocator_proto.h"
#include "user_api.h"

#if CAP_ENABLE

/*============================================================================
 * 内部工具
 *============================================================================*/

static void alloc_release_caps(cap_id_t *caps, uint8_t count) {
    uint8_t i;
    if (caps == NULL) {
        return;
    }
    for (i = 0; i < count && i < IPC_CAPS_MAX; i++) {
        if (caps[i] > 0) {
            (void)sys_cap_revoke(caps[i]);
            caps[i] = KERN_INVALID_ID;
        }
    }
}

static int alloc_reply_status(int ep_cap, alloc_msg_t *msg, int status) {
    msg->status = status;
    return sys_ep_reply(ep_cap, msg);
}

/*============================================================================
 * 服务循环
 *============================================================================

 * @param ep_cap        allocator 的 endpoint cap (READ|WRITE)
 * @param max_requests  0 = 永久循环; >0 = 处理 N 个请求后退出
 */
int allocator_service_run(int ep_cap, uint32_t max_requests) {
    uint8_t msg_buf[KERN_EP_MSG_SIZE];
    alloc_msg_t *msg = (alloc_msg_t *)msg_buf;
    cap_id_t caps[IPC_CAPS_MAX];
    ipc_cap_xfer_t xfers[IPC_CAPS_MAX];
    uint8_t cap_count = 0;
    int err = KERN_OK;

    if (ep_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    for (uint32_t round = 0;
         (max_requests == 0U || round < max_requests) && err == KERN_OK;
         round++) {

        /* 每轮清空缓冲和 cap 槽 */
        uint32_t i;
        uint8_t *p = msg_buf;
        for (i = 0; i < sizeof(msg_buf); i++) {
            p[i] = 0;
        }
        for (i = 0; i < IPC_CAPS_MAX; i++) {
            caps[i] = KERN_INVALID_ID;
            xfers[i].src_cap = KERN_INVALID_ID;
            xfers[i].rights = 0;
            xfers[i].flags = IPC_CAP_COPY;
        }
        cap_count = 0;

        /* 收请求 (带 cap,client 会附上 inbox cap) */
        err = sys_ep_recv_caps(ep_cap, msg_buf, caps, &cap_count, 1000);
        if (err == KERN_ERR_TIMEOUT && max_requests == 0U) {
            err = KERN_OK;
            continue;
        }
        if (err != KERN_OK) {
            break;
        }

        /* 校验协议头 */
        if (msg->magic != ALLOC_MAGIC || !alloc_opcode_valid(msg->opcode)) {
            alloc_release_caps(caps, cap_count);
            err = alloc_reply_status(ep_cap, msg, KERN_ERR_PARAM);
            continue;
        }

        /* PING: 直接回 OK */
        if (msg->opcode == ALLOC_OP_PING) {
            alloc_release_caps(caps, cap_count);
            err = alloc_reply_status(ep_cap, msg, KERN_OK);
            continue;
        }

        /* CREATE: 需要 client 的 inbox cap (caps[0]) 来回传 shm cap */
        if (msg->opcode == ALLOC_OP_CREATE) {
            if (cap_count != 1 || caps[0] <= 0) {
                alloc_release_caps(caps, cap_count);
                err = alloc_reply_status(ep_cap, msg, KERN_ERR_CAP);
                continue;
            }
            if (msg->size < 32U) {
                alloc_release_caps(caps, cap_count);
                err = alloc_reply_status(ep_cap, msg, KERN_ERR_PARAM);
                continue;
            }

            /* 先 reply OK,让 client 解除 send 阻塞 */
            err = alloc_reply_status(ep_cap, msg, KERN_OK);
            if (err != KERN_OK) {
                alloc_release_caps(caps, cap_count);
                continue;
            }

            /* 创建 shm。allocator 是特权任务,sys_shm_create 允许。
             * rights 带 TRANSFER,否则后续推给 client 会失败。 */
            int shm_cap = sys_shm_create((int)msg->size,
                                         (int)(CAP_READ | CAP_WRITE |
                                               CAP_TRANSFER | CAP_GRANT));
            if (shm_cap <= 0) {
                /* 创建失败:通知 client (发一个带错误 status 的消息) */
                alloc_msg_init(msg, ALLOC_OP_CREATE, msg->seq);
                msg->status = shm_cap;   /* 错误码 */
                msg->result = KERN_INVALID_ID;
                (void)sys_ep_send_caps(caps[0], msg_buf, xfers, 0, 1000);
                alloc_release_caps(caps, cap_count);
                continue;
            }

            /* 把 shm cap copy 到 client 的 inbox。这里必须用 COPY，因为
             * allocator 仍要保留源 cap，统一管理 backing 生命周期；M2
             * capability transaction 的 MOVE 会原子转移 cap 本身。
             * client 持有 copy 独立使用。client 用完自己 revoke 它的 copy。
             * allocator 源 cap 在服务退出时由 cap_revoke_all 统一回收。
             * rights:RW + TRANSFER;GRANT 不给,防权限放大。 */
            alloc_msg_init(msg, ALLOC_OP_CREATE, msg->seq);
            msg->status = KERN_OK;
            msg->result = shm_cap;
            xfers[0].src_cap = (cap_id_t)shm_cap;
            xfers[0].rights = CAP_READ | CAP_WRITE | CAP_TRANSFER;
            xfers[0].flags = IPC_CAP_COPY;
            err = sys_ep_send_caps(caps[0], msg_buf, xfers, 1, 1000);

            /* 释放 client 随请求发来的 inbox cap 副本 (caps[0])。
             * 不 revoke shm_cap (源),保留给 allocator 以免 backing 被 kfree。 */
            alloc_release_caps(caps, cap_count);
            continue;
        }

        /* DESTROY: 目前 client 直接用 sys_cap_revoke 即可,这里留作扩展 */
        if (msg->opcode == ALLOC_OP_DESTROY) {
            alloc_release_caps(caps, cap_count);
            err = alloc_reply_status(ep_cap, msg, KERN_OK);
            continue;
        }

        /* 未知 opcode */
        alloc_release_caps(caps, cap_count);
        err = alloc_reply_status(ep_cap, msg, KERN_ERR_PARAM);
    }

    return err;
}

/*============================================================================
 * 客户端 API
 *============================================================================*/

int allocator_ping(int alloc_ep_cap, uint32_t timeout) {
    alloc_msg_t msg;
    int err;

    alloc_msg_init(&msg, ALLOC_OP_PING, 0);
    err = sys_ep_send(alloc_ep_cap, &msg, (int)timeout);
    if (err != KERN_OK) {
        return err;
    }
    /* reply 覆盖 msg,检查 status */
    if (msg.magic != ALLOC_MAGIC) {
        return KERN_ERR_PARAM;
    }
    return msg.status;
}

int allocator_create_shm(int alloc_ep_cap, int inbox_cap,
                         uint32_t size, uint32_t rights,
                         int *out_shm_cap, uint32_t timeout) {
    alloc_msg_t msg;
    ipc_cap_xfer_t xfers[IPC_CAPS_MAX];
    cap_id_t recv_caps[IPC_CAPS_MAX];
    uint8_t cap_count = 0;
    int err;
    uint32_t i;

    if (out_shm_cap == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_shm_cap = KERN_INVALID_ID;
    (void)rights;  /* 当前统一 RW,预留扩展 */

    /* 随请求附上 inbox cap,allocator 用它推 shm cap 回来 */
    for (i = 0; i < IPC_CAPS_MAX; i++) {
        xfers[i].src_cap = KERN_INVALID_ID;
        xfers[i].rights = 0;
        xfers[i].flags = IPC_CAP_COPY;
    }
    xfers[0].src_cap = (cap_id_t)inbox_cap;
    xfers[0].rights = CAP_READ | CAP_WRITE;
    xfers[0].flags = IPC_CAP_COPY;

    alloc_msg_init(&msg, ALLOC_OP_CREATE, 0);
    msg.size = size;
    msg.rights = ALLOC_RIGHTS_RW;

    /* 发请求 (带 inbox cap) */
    err = sys_ep_send_caps(alloc_ep_cap, &msg, xfers, 1, (int)timeout);
    if (err != KERN_OK) {
        return err;
    }

    /* allocator 先 reply OK (msg 被覆盖) */
    if (msg.magic != ALLOC_MAGIC || msg.status != KERN_OK) {
        return (msg.magic == ALLOC_MAGIC) ? msg.status : KERN_ERR_PARAM;
    }

    /* 在 inbox 上等 allocator 推来的 shm cap */
    for (i = 0; i < IPC_CAPS_MAX; i++) {
        recv_caps[i] = KERN_INVALID_ID;
    }
    err = sys_ep_recv_caps(inbox_cap, &msg, recv_caps, &cap_count, (int)timeout);
    if (err != KERN_OK) {
        return err;
    }
    if (cap_count != 1 || recv_caps[0] <= 0) {
        return KERN_ERR_CAP;
    }

    *out_shm_cap = (int)recv_caps[0];
    return KERN_OK;
}

#endif /* CAP_ENABLE */
