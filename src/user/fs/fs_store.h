/**
 * @file fs_store.h
 * @brief fs_server 内部 FS 存储 — 自管 inode 池 + ramfs (Phase B)
 *
 * 这是 fs_server 进程内部的文件系统实现,取代代理式阶段调 open/read 内联 syscall。
 * inode 池和 ramfs 数据分配在 sys_mem_alloc 申请的 memblock 里 (经 sys_mem_map
 * 映射进 fs_server 的 MPU region 3-7),不依赖内核 VFS。
 *
 * 根目录树:fs_store_init 构造 "/" (DIR) + "/tmp" (DIR, ramfs) + "/dev" (DIR)。
 * Phase B 的 devfs 转发在 fs_server_run 里处理,fs_store 只管 ramfs 部分。
 */

#ifndef FS_STORE_H
#define FS_STORE_H

#include "fs_proto.h"
#include <stdint.h>

/* inode 类型 (与内核 inode.h 一致,但 fs_server 自管,不依赖内核 inode.h) */
#define FS_INODE_FILE   0
#define FS_INODE_DIR    1
#define FS_INODE_CHRDEV 2   /* devfs 设备节点 (Phase B devfs 转发用) */

#define FS_NAME_LEN     32
#define FS_STORE_MAX_INODES  32

/* open flags (与内核一致) */
#define FS_O_RDONLY  0x01
#define FS_O_WRONLY  0x02
#define FS_O_RDWR    0x03
#define FS_O_CREAT   0x04
#define FS_O_TRUNC   0x08

/* lseek whence */
#define FS_SEEK_SET  0
#define FS_SEEK_CUR  1
#define FS_SEEK_END  2

/* fs_server 内部 inode (简化版,不含内核的 ops 表指针,直接用类型分派) */
typedef struct fs_inode {
    uint32_t  ino;
    char      name[FS_NAME_LEN];
    uint8_t   type;            /* FS_INODE_* */
    uint32_t  size;            /* FILE: 数据大小 */
    uint32_t  refcount;

    /* ramfs 文件数据 (FILE 类型) */
    uint8_t  *data_buf;        /* 数据缓冲 (在 store 内存里) */
    uint32_t  data_cap;        /* 数据缓冲容量 */

    /* devfs 设备 (CHRDEV 类型,Phase B devfs 转发) */
    int       dev_ep_cap;      /* 设备对应的 driver server endpoint cap */

    struct fs_inode *parent;
    struct fs_inode *children;       /* 子节点链表头 */
    struct fs_inode *next_sibling;
} fs_inode_t;

/* 目录条目 (readdir 返回) */
typedef struct {
    uint32_t ino;
    char     name[FS_NAME_LEN];
    uint8_t  type;
} fs_dirent_t;

/* stat 结果 */
typedef struct {
    uint32_t ino;
    uint32_t size;
    uint8_t  type;
} fs_statinfo_t;

/**
 * 初始化 fs_store:分配 memblock、构造 / /tmp /dev 目录树。
 * mem_cap 是 fs_server 启动时 sys_mem_alloc 得到的 cap (已 sys_mem_map)。
 * store_buf 是映射后的指针,store_size 是大小。
 * 返回 KERN_OK 或错误码。
 */
int fs_store_init(void *store_buf, uint32_t store_size);

/* 路径解析:返回 inode 指针 (带 ref+1),失败返回 NULL */
fs_inode_t *fs_store_lookup(const char *path);

/* 释放 lookup 返回的引用 */
void fs_store_put(fs_inode_t *inode);

/* 文件操作:打开 (返回 fd token, 0..FS_STORE_MAX_FDS-1) */
int fs_store_open(const char *path, uint32_t flags);  /* 返回 fd token 或负错误 */
int fs_store_close(int fd);
int32_t fs_store_read(int fd, void *buf, uint32_t len);
int32_t fs_store_write(int fd, const void *buf, uint32_t len);
int fs_store_lseek(int fd, int32_t offset, uint32_t whence);
int fs_store_readdir(int fd, fs_dirent_t *entry);

/* 目录/路径操作 */
int fs_store_mkdir(const char *path);
int fs_store_unlink(const char *path);
int fs_store_stat(const char *path, fs_statinfo_t *st);

/* devfs:注册设备节点 (Phase B:fs_server 启动时调用) */
int fs_store_register_dev(const char *name, int dev_ep_cap);

/* fd 表大小 */
#define FS_STORE_MAX_FDS  8

#endif /* FS_STORE_H */
