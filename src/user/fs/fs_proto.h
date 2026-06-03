/**
 * @file fs_proto.h
 * @brief User-space file-service IPC protocol
 */

#ifndef FS_PROTO_H
#define FS_PROTO_H

#include "kernel_types.h"
#include "inode.h"
#include <stdint.h>

#define FS_MAGIC        0x46535631U
#define FS_PATH_MAX     24U
#define FS_PAYLOAD_MAX  8U
#define FS_FD_MAX       4U

#define FS_OP_PING      1U
#define FS_OP_OPEN      2U
#define FS_OP_CLOSE     3U
#define FS_OP_READ      4U
#define FS_OP_WRITE     5U
#define FS_OP_LSEEK     6U
#define FS_OP_READDIR   7U
#define FS_OP_UNLINK    8U
#define FS_OP_MKDIR     9U
#define FS_OP_STAT      10U

typedef struct {
    uint32_t magic;
    uint16_t opcode;
    uint16_t flags;
    uint32_t seq;
    int32_t status;
    int32_t result;
    int32_t fd;
    uint32_t length;
    int32_t offset;
    char path[FS_PATH_MAX];
    uint8_t payload[FS_PAYLOAD_MAX];
} fs_msg_t;

int fs_opcode_valid(uint16_t opcode);
void fs_msg_init(fs_msg_t *msg, uint16_t opcode, uint32_t seq);

int fs_ping(int ep_cap, uint32_t timeout);
int fs_open(int ep_cap, const char *path, uint32_t flags, uint32_t timeout);
int fs_close(int ep_cap, int fd, uint32_t timeout);
int fs_read(int ep_cap, int fd, void *buf, uint32_t len, uint32_t timeout);
int fs_write(int ep_cap, int fd, const void *buf, uint32_t len,
             uint32_t timeout);
int fs_lseek(int ep_cap, int fd, int32_t offset, uint32_t whence,
             uint32_t timeout);
int fs_readdir(int ep_cap, int fd, dirent_t *entry, uint32_t timeout);
int fs_unlink(int ep_cap, const char *path, uint32_t timeout);
int fs_mkdir(int ep_cap, const char *path, uint32_t timeout);
int fs_stat(int ep_cap, const char *path, vfs_stat_t *st, uint32_t timeout);
const char *fs_error_name(int err);

int fs_server_run(int ep_cap, uint32_t max_requests);

#endif /* FS_PROTO_H */
