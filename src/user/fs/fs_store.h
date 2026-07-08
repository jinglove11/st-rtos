/**
 * @file fs_store.h
 * @brief fs_server 内部 FS 存储 — 自管 inode 池 + ramfs (Phase B, context-based)
 *
 * fs_server 是 user 任务,不能用 .bss 全局 (MPU Region 1 禁用)。
 * 所有状态打包进 fs_store_ctx_t,放在 sys_mem_alloc 的 memblock 里 (经
 * sys_mem_map 映射进 fs_server 的 MPU region)。fs_store 函数接受 ctx 指针。
 */

#ifndef FS_STORE_H
#define FS_STORE_H

#include "fs_proto.h"
#include <stdint.h>

#define FS_INODE_FILE   0
#define FS_INODE_DIR    1
#define FS_INODE_CHRDEV 2

#define FS_NAME_LEN     32
#define FS_STORE_MAX_INODES  32
#define FS_STORE_MAX_FDS     8

#define FS_O_RDONLY  0x01
#define FS_O_WRONLY  0x02
#define FS_O_RDWR    0x03
#define FS_O_CREAT   0x04
#define FS_O_TRUNC   0x08

#define FS_SEEK_SET  0
#define FS_SEEK_CUR  1
#define FS_SEEK_END  2

typedef struct fs_inode {
    uint32_t  ino;
    char      name[FS_NAME_LEN];
    uint8_t   type;
    uint32_t  size;
    uint32_t  refcount;

    uint8_t  *data_buf;
    uint32_t  data_cap;

    int       dev_ep_cap;          /* CHRDEV:driver server endpoint cap */

    struct fs_inode *parent;
    struct fs_inode *children;
    struct fs_inode *next_sibling;
} fs_inode_t;

typedef struct {
    uint32_t ino;
    char     name[FS_NAME_LEN];
    uint8_t  type;
} fs_dirent_t;

typedef struct {
    uint32_t ino;
    uint32_t size;
    uint8_t  type;
} fs_statinfo_t;

typedef struct {
    uint8_t   in_use;
    fs_inode_t *inode;
    uint32_t  offset;
} fs_fd_t;

/* fs_store 上下文 (放在 memblock 里,所有状态集中于此) */
typedef struct {
    uint8_t  *store_base;          /* memblock 映射后的基址 */
    uint32_t  store_size;
    uint32_t  store_off;           /* bump allocator 下一个偏移 */

    fs_inode_t *inode_pool;        /* inode 池数组 */
    uint32_t  ino_counter;

    fs_inode_t *root;              /* "/" */
    fs_inode_t *tmp_dir;           /* "/tmp" */
    fs_inode_t *dev_dir;           /* "/dev" */

    fs_fd_t  fds[FS_STORE_MAX_FDS];
} fs_store_ctx_t;

/**
 * 初始化:在 store_buf (已 sys_mem_map 的 memblock) 开头放 ctx,
 * 构造 / /tmp /dev 目录树。
 * store_size 必须 >= sizeof(fs_store_ctx_t) + inode池 + 余量。
 * 返回 ctx 指针 (成功) 或 NULL。
 */
fs_store_ctx_t *fs_store_init(void *store_buf, uint32_t store_size);

/* 所有操作通过 ctx,线程不安全 (fs_server 单线程) */
fs_inode_t *fs_store_lookup(fs_store_ctx_t *ctx, const char *path);
void fs_store_put(fs_store_ctx_t *ctx, fs_inode_t *inode);

int fs_store_open(fs_store_ctx_t *ctx, const char *path, uint32_t flags);
int fs_store_close(fs_store_ctx_t *ctx, int fd);
int32_t fs_store_read(fs_store_ctx_t *ctx, int fd, void *buf, uint32_t len);
int32_t fs_store_write(fs_store_ctx_t *ctx, int fd, const void *buf, uint32_t len);
int fs_store_lseek(fs_store_ctx_t *ctx, int fd, int32_t offset, uint32_t whence);
int fs_store_readdir(fs_store_ctx_t *ctx, int fd, fs_dirent_t *entry);

/* 查询 fd 对应的设备 endpoint cap (CHRDEV 用)。
 * 返回 >0:dev_ep_cap;<=0:不是 CHRDEV 或无效 fd。 */
int fs_store_fd_dev_ep(fs_store_ctx_t *ctx, int fd);

int fs_store_mkdir(fs_store_ctx_t *ctx, const char *path);
int fs_store_unlink(fs_store_ctx_t *ctx, const char *path);
int fs_store_stat(fs_store_ctx_t *ctx, const char *path, fs_statinfo_t *st);

int fs_store_register_dev(fs_store_ctx_t *ctx, const char *name, int dev_ep_cap);

#endif /* FS_STORE_H */
