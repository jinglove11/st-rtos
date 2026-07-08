/**
 * @file fs_server.c
 * @brief Minimal user-space FS server over endpoint IPC
 */

#include "fs_proto.h"
#include "user_api.h"
#include "inode.h"
#include "fs_store.h"
#include "driver_proto.h"
#include "fault_endpoint.h"
#include <stdint.h>

#if VFS_ENABLE && CAP_ENABLE

int fs_opcode_valid(uint16_t opcode) {
    return opcode >= FS_OP_PING && opcode <= FS_OP_LOOKUP;
}

void fs_msg_init(fs_msg_t *msg, uint16_t opcode, uint32_t seq) {
    if (msg == NULL) {
        return;
    }
    for (uint32_t i = 0; i < sizeof(*msg); i++) {
        ((uint8_t *)msg)[i] = 0;
    }
    msg->magic = FS_MAGIC;
    msg->opcode = opcode;
    msg->seq = seq;
    msg->fd = KERN_INVALID_ID;
}

static void fs_copy_path(char *dst, const char *src) {
    for (uint32_t i = 0; i < FS_PATH_MAX; i++) {
        dst[i] = src[i];
        if (src[i] == '\0') {
            return;
        }
    }
    dst[FS_PATH_MAX - 1U] = '\0';
}

static int fs_path_valid(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    for (uint32_t i = 0; i < FS_PATH_MAX; i++) {
        if (path[i] == '\0') {
            return 1;
        }
    }
    return 0;
}

static void fs_copy_dirent_name(char *dst, const char *src) {
    for (uint32_t i = 0; i < FS_PATH_MAX; i++) {
        dst[i] = src[i];
        if (src[i] == '\0') {
            return;
        }
    }
    dst[FS_PATH_MAX - 1U] = '\0';
}

static void fs_restore_dirent_name(char *dst, const char *src) {
    for (uint32_t i = 0; i < INODE_NAME_LEN; i++) {
        dst[i] = '\0';
    }
    for (uint32_t i = 0; i < FS_PATH_MAX && i < INODE_NAME_LEN; i++) {
        dst[i] = src[i];
        if (src[i] == '\0') {
            return;
        }
    }
    dst[INODE_NAME_LEN - 1U] = '\0';
}

static int fs_check_reply(const fs_msg_t *msg, uint16_t opcode) {
    if (msg->magic != FS_MAGIC || msg->opcode != opcode) {
        return KERN_ERR_STATE;
    }
    return msg->status;
}

static int fs_send_simple(int ep_cap, fs_msg_t *msg, uint32_t timeout) {
    int err = sys_ep_send(ep_cap, msg, (int)timeout);
    if (err != KERN_OK) {
        return err;
    }
    return fs_check_reply(msg, msg->opcode);
}

int fs_ping(int ep_cap, uint32_t timeout) {
    fs_msg_t msg;

    if (ep_cap <= 0) {
        return KERN_ERR_PARAM;
    }
    fs_msg_init(&msg, FS_OP_PING, 0);
    return fs_send_simple(ep_cap, &msg, timeout);
}

int fs_open(int ep_cap, const char *path, uint32_t flags, uint32_t timeout) {
    fs_msg_t msg;
    int err;

    if (ep_cap <= 0 || !fs_path_valid(path)) {
        return KERN_ERR_PARAM;
    }
    fs_msg_init(&msg, FS_OP_OPEN, 0);
    msg.flags = (uint16_t)flags;
    fs_copy_path(msg.path, path);
    err = fs_send_simple(ep_cap, &msg, timeout);
    if (err != KERN_OK) {
        return err;
    }
    return msg.result;
}

int fs_close(int ep_cap, int fd, uint32_t timeout) {
    fs_msg_t msg;

    if (ep_cap <= 0 || fd <= 0) {
        return KERN_ERR_PARAM;
    }
    fs_msg_init(&msg, FS_OP_CLOSE, 0);
    msg.fd = fd;
    return fs_send_simple(ep_cap, &msg, timeout);
}

int fs_read(int ep_cap, int fd, void *buf, uint32_t len, uint32_t timeout) {
    fs_msg_t msg;
    uint8_t *dst = (uint8_t *)buf;
    int err;

    if (ep_cap <= 0 || fd <= 0 || (buf == NULL && len > 0U) ||
        len > FS_PAYLOAD_MAX) {
        return KERN_ERR_PARAM;
    }
    if (dst != NULL) {
        for (uint32_t i = 0; i < len; i++) {
            dst[i] = 0;
        }
    }
    fs_msg_init(&msg, FS_OP_READ, 0);
    msg.fd = fd;
    msg.length = len;
    err = fs_send_simple(ep_cap, &msg, timeout);
    if (err != KERN_OK) {
        return err;
    }
    if (msg.result < 0 || (uint32_t)msg.result > len ||
        (uint32_t)msg.result > FS_PAYLOAD_MAX) {
        return KERN_ERR_OVERFLOW;
    }
    for (uint32_t i = 0; i < (uint32_t)msg.result; i++) {
        dst[i] = msg.payload[i];
    }
    return msg.result;
}

int fs_write(int ep_cap, int fd, const void *buf, uint32_t len,
             uint32_t timeout) {
    fs_msg_t msg;
    const uint8_t *src = (const uint8_t *)buf;
    int err;

    if (ep_cap <= 0 || fd <= 0 || (buf == NULL && len > 0U) ||
        len > FS_PAYLOAD_MAX) {
        return KERN_ERR_PARAM;
    }
    fs_msg_init(&msg, FS_OP_WRITE, 0);
    msg.fd = fd;
    msg.length = len;
    for (uint32_t i = 0; i < len; i++) {
        msg.payload[i] = src[i];
    }
    err = fs_send_simple(ep_cap, &msg, timeout);
    if (err != KERN_OK) {
        return err;
    }
    return msg.result;
}

int fs_lseek(int ep_cap, int fd, int32_t offset, uint32_t whence,
             uint32_t timeout) {
    fs_msg_t msg;
    int err;

    if (ep_cap <= 0 || fd <= 0 || whence > SEEK_END) {
        return KERN_ERR_PARAM;
    }
    fs_msg_init(&msg, FS_OP_LSEEK, 0);
    msg.fd = fd;
    msg.flags = (uint16_t)whence;
    msg.offset = offset;
    err = fs_send_simple(ep_cap, &msg, timeout);
    if (err != KERN_OK) {
        return err;
    }
    return msg.result;
}

int fs_readdir(int ep_cap, int fd, dirent_t *entry, uint32_t timeout) {
    fs_msg_t msg;
    int err;

    if (ep_cap <= 0 || fd <= 0 || entry == NULL) {
        return KERN_ERR_PARAM;
    }
    entry->ino = 0;
    entry->type = 0;
    for (uint32_t i = 0; i < INODE_NAME_LEN; i++) {
        entry->name[i] = '\0';
    }

    fs_msg_init(&msg, FS_OP_READDIR, 0);
    msg.fd = fd;
    err = fs_send_simple(ep_cap, &msg, timeout);
    if (err != KERN_OK) {
        return err;
    }
    entry->ino = (uint32_t)msg.result;
    entry->type = (uint8_t)msg.length;
    fs_restore_dirent_name(entry->name, msg.path);
    return KERN_OK;
}

int fs_unlink(int ep_cap, const char *path, uint32_t timeout) {
    fs_msg_t msg;

    if (ep_cap <= 0 || !fs_path_valid(path)) {
        return KERN_ERR_PARAM;
    }
    fs_msg_init(&msg, FS_OP_UNLINK, 0);
    fs_copy_path(msg.path, path);
    return fs_send_simple(ep_cap, &msg, timeout);
}

int fs_mkdir(int ep_cap, const char *path, uint32_t timeout) {
    fs_msg_t msg;

    if (ep_cap <= 0 || !fs_path_valid(path)) {
        return KERN_ERR_PARAM;
    }
    fs_msg_init(&msg, FS_OP_MKDIR, 0);
    fs_copy_path(msg.path, path);
    return fs_send_simple(ep_cap, &msg, timeout);
}

int fs_stat(int ep_cap, const char *path, vfs_stat_t *st, uint32_t timeout) {
    fs_msg_t msg;
    int err;

    if (ep_cap <= 0 || !fs_path_valid(path) || st == NULL) {
        return KERN_ERR_PARAM;
    }
    st->ino = 0;
    st->size = 0;
    st->type = 0;

    fs_msg_init(&msg, FS_OP_STAT, 0);
    fs_copy_path(msg.path, path);
    err = fs_send_simple(ep_cap, &msg, timeout);
    if (err != KERN_OK) {
        return err;
    }
    st->ino = (uint32_t)msg.result;
    st->size = msg.length;
    st->type = (uint8_t)msg.flags;
    return KERN_OK;
}

int fs_lookup(int ep_cap, const char *path, vfs_stat_t *st, uint32_t timeout) {
    /* LOOKUP 与 STAT 在语义上一致 (返回路径元数据,不打开 fd)。
     * 当前代理式 fs_server 把它转发成 vfs_stat。
     * Phase B 服务化后,fs_server 自管 inode 树,LOOKUP 走自己的路径解析。 */
    return fs_stat(ep_cap, path, st, timeout);
}

const char *fs_error_name(int err) {
    switch (err) {
    case KERN_OK:
        return "ok";
    case KERN_ERR_PARAM:
        return "param";
    case KERN_ERR_TIMEOUT:
        return "timeout";
    case KERN_ERR_RESOURCE:
        return "resource";
    case KERN_ERR_STATE:
        return "state";
    case KERN_ERR_CAP:
        return "cap";
    case KERN_ERR_BUSY:
        return "busy";
    case KERN_ERR_NOEXIST:
        return "noexist";
    case KERN_ERR_OVERFLOW:
        return "overflow";
    case KERN_ERR_PERM:
        return "perm";
    case KERN_ERR_NOTDIR:
        return "notdir";
    case KERN_ERR_ISDIR:
        return "isdir";
    case KERN_ERR_FAULT:
        return "fault";
    default:
        return "unknown";
    }
}

static int fs_reply(int ep_cap, fs_msg_t *msg, int status, int result) {
    msg->status = status;
    msg->result = result;
    return sys_ep_reply(ep_cap, msg);
}

int fs_server_run(int ep_cap, uint32_t max_requests) {
    return fs_server_run_with_dev(ep_cap, max_requests, 0, NULL);
}

int fs_server_run_with_dev(int ep_cap, uint32_t max_requests,
                           int dev_ep_cap, const char *dev_name) {
    fs_msg_t msg;
    int err = KERN_OK;
    int fault_ep_cap = -1;   /* kern.fault endpoint cap (客户端死亡清理) */

    if (ep_cap <= 0) {
        return KERN_ERR_PARAM;
    }

    /* Phase B:fs_server 自管 inode 池 + ramfs。 */
    int store_mem_cap = sys_mem_alloc(4096);
    int init_err = KERN_OK;
    fs_store_ctx_t *ctx = NULL;
    void *store = NULL;

    if (store_mem_cap <= 0) {
        init_err = store_mem_cap;
    } else {
        store = sys_mem_map(store_mem_cap, CAP_READ | CAP_WRITE);
        if ((intptr_t)store <= 0) {
            init_err = (int)(intptr_t)store;
            (void)sys_mem_free(store_mem_cap);
            store_mem_cap = -1;
        } else {
            ctx = fs_store_init(store, 4096);
            if (ctx == NULL) {
                init_err = -77;
                (void)sys_mem_free(store_mem_cap);
                store_mem_cap = -1;
            } else if (dev_ep_cap > 0 && dev_name != NULL) {
                /* 注册 devfs 设备节点 /dev/<dev_name> → driver server */
                int reg_err = fs_store_register_dev(ctx, dev_name, dev_ep_cap);
                if (reg_err != KERN_OK) {
                    init_err = reg_err;
                }
            }
        }
    }

    /* 订阅 kern.fault:客户端崩溃时按 task_id 清理它的 fd。
     * sys_fault_subscribe 返回 kern.fault endpoint cap。失败则跳过
     * (fs_server 仍工作,但无客户端死亡清理)。 */
    if (ctx != NULL) {
        fault_ep_cap = sys_fault_subscribe();
    }

    for (uint32_t round = 0;
         (max_requests == 0U || round < max_requests) && err == KERN_OK;
         round++) {
        /* Phase C3:poll kern.fault,清理崩溃客户端的 fd (精确清理)。
         * timeout=0 非阻塞:有事件就处理,无事件立即返回。 */
        if (ctx != NULL && fault_ep_cap > 0) {
            fault_event_t fevt;
            int ferr = sys_ep_recv(fault_ep_cap, &fevt, 0);
            if (ferr == KERN_OK) {
                /* 客户端崩溃:按 task_id 关闭它所有 fd */
                (void)fs_store_close_client_fds(ctx, (int)fevt.task_id);
            }
        }

        fs_msg_init(&msg, 0, 0);
        err = sys_ep_recv(ep_cap, &msg, 1000);
        if (err == KERN_ERR_TIMEOUT && max_requests == 0U) {
            err = KERN_OK;
            continue;
        }
        if (err != KERN_OK) {
            break;
        }

        if (msg.magic != FS_MAGIC || !fs_opcode_valid(msg.opcode)) {
            err = fs_reply(ep_cap, &msg, KERN_ERR_PARAM, 0);
            continue;
        }

        /* 初始化失败时:所有请求返回 init_err (暴露具体错误码供诊断) */
        if (ctx == NULL) {
            err = fs_reply(ep_cap, &msg, init_err, 0);
            continue;
        }

        if (msg.opcode == FS_OP_PING) {
            err = fs_reply(ep_cap, &msg, KERN_OK, 0);
        } else if (msg.opcode == FS_OP_OPEN) {
            if (!fs_path_valid(msg.path)) {
                err = fs_reply(ep_cap, &msg, KERN_ERR_PARAM, 0);
            } else {
                int fd = fs_store_open(ctx, msg.path, msg.flags);
                if (fd < 0) {
                    err = fs_reply(ep_cap, &msg, fd, 0);
                } else {
                    /* 记录 fd 归属:谁打开的 (客户端死亡时清理) */
                    int sender = sys_ep_sender(ep_cap);
                    fs_store_fd_set_client(ctx, fd, sender);
                    err = fs_reply(ep_cap, &msg, KERN_OK, fd);
                }
            }
        } else if (msg.opcode == FS_OP_CLOSE) {
            int close_err = fs_store_close(ctx, msg.fd);
            err = fs_reply(ep_cap, &msg, close_err, 0);
        } else if (msg.opcode == FS_OP_READ) {
            if (msg.length > FS_PAYLOAD_MAX) {
                err = fs_reply(ep_cap, &msg, KERN_ERR_PARAM, 0);
            } else {
                int n = fs_store_read(ctx, msg.fd, msg.payload, msg.length);
                if (n == -16) {
                    /* CHRDEV:转发给 driver server */
                    int dev_ep = fs_store_fd_dev_ep(ctx, msg.fd);
                    if (dev_ep <= 0) {
                        err = fs_reply(ep_cap, &msg, KERN_ERR_STATE, 0);
                    } else {
                        int rn = driver_read(dev_ep, msg.payload,
                                             msg.length, 1000);
                        err = fs_reply(ep_cap, &msg,
                                       rn < 0 ? rn : KERN_OK,
                                       rn < 0 ? 0 : rn);
                    }
                } else {
                    err = fs_reply(ep_cap, &msg, n < 0 ? n : KERN_OK,
                                   n < 0 ? 0 : n);
                }
            }
        } else if (msg.opcode == FS_OP_WRITE) {
            if (msg.length > FS_PAYLOAD_MAX) {
                err = fs_reply(ep_cap, &msg, KERN_ERR_PARAM, 0);
            } else {
                int n = fs_store_write(ctx, msg.fd, msg.payload, msg.length);
                if (n == -16) {
                    /* CHRDEV:转发给 driver server */
                    int dev_ep = fs_store_fd_dev_ep(ctx, msg.fd);
                    if (dev_ep <= 0) {
                        err = fs_reply(ep_cap, &msg, KERN_ERR_STATE, 0);
                    } else {
                        int wn = driver_write(dev_ep, msg.payload,
                                              msg.length, 1000);
                        err = fs_reply(ep_cap, &msg,
                                       wn < 0 ? wn : KERN_OK,
                                       wn < 0 ? 0 : wn);
                    }
                } else {
                    err = fs_reply(ep_cap, &msg, n < 0 ? n : KERN_OK,
                                   n < 0 ? 0 : n);
                }
            }
        } else if (msg.opcode == FS_OP_LSEEK) {
            int pos = fs_store_lseek(ctx, msg.fd, msg.offset, msg.flags);
            err = fs_reply(ep_cap, &msg, pos < 0 ? pos : KERN_OK,
                           pos < 0 ? 0 : pos);
        } else if (msg.opcode == FS_OP_READDIR) {
            fs_dirent_t entry;
            int readdir_err = fs_store_readdir(ctx, msg.fd, &entry);
            if (readdir_err != KERN_OK) {
                err = fs_reply(ep_cap, &msg, readdir_err, 0);
            } else {
                msg.length = entry.type;
                fs_copy_dirent_name(msg.path, entry.name);
                err = fs_reply(ep_cap, &msg, KERN_OK, (int)entry.ino);
            }
        } else if (msg.opcode == FS_OP_UNLINK) {
            if (!fs_path_valid(msg.path)) {
                err = fs_reply(ep_cap, &msg, KERN_ERR_PARAM, 0);
            } else {
                int unlink_err = fs_store_unlink(ctx, msg.path);
                err = fs_reply(ep_cap, &msg, unlink_err, 0);
            }
        } else if (msg.opcode == FS_OP_MKDIR) {
            if (!fs_path_valid(msg.path)) {
                err = fs_reply(ep_cap, &msg, KERN_ERR_PARAM, 0);
            } else {
                int mkdir_err = fs_store_mkdir(ctx, msg.path);
                err = fs_reply(ep_cap, &msg, mkdir_err, 0);
            }
        } else if (msg.opcode == FS_OP_STAT) {
            if (!fs_path_valid(msg.path)) {
                err = fs_reply(ep_cap, &msg, KERN_ERR_PARAM, 0);
            } else {
                fs_statinfo_t st;
                int stat_err = fs_store_stat(ctx, msg.path, &st);
                msg.flags = st.type;
                msg.length = st.size;
                err = fs_reply(ep_cap, &msg, stat_err,
                               stat_err == KERN_OK ? (int)st.ino : 0);
            }
        } else if (msg.opcode == FS_OP_LOOKUP) {
            /* LOOKUP:路径元数据查询 (Phase B 走自管路径解析) */
            if (!fs_path_valid(msg.path)) {
                err = fs_reply(ep_cap, &msg, KERN_ERR_PARAM, 0);
            } else {
                fs_statinfo_t st;
                int stat_err = fs_store_stat(ctx, msg.path, &st);
                msg.flags = st.type;
                msg.length = st.size;
                err = fs_reply(ep_cap, &msg, stat_err,
                               stat_err == KERN_OK ? (int)st.ino : 0);
            }
        } else {
            err = fs_reply(ep_cap, &msg, KERN_ERR_STATE, 0);
        }
    }

    /* fs_server 的 fd 表在 ctx 里,随 memblock 释放自动消失 */
    if (store_mem_cap > 0) {
        (void)sys_mem_free(store_mem_cap);
    }
    return err;
}

#endif /* VFS_ENABLE && CAP_ENABLE */
