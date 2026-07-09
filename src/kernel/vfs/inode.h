/**
 * @file inode.h
 * @brief VFS inode 系统 — 核心抽象 + 操作表类型
 *
 * inode 是 VFS 核心抽象，类型决定操作表绑定:
 *   FILE   → ops_u.file_ops  (普通文件)
 *   CHRDEV → ops_u.cdev_ops  (字符设备, 通过 private_data 二级分发到 dev_ops_t)
 *   DIR    → dir_ops         (目录, 独立字段不在 union 中)
 *
 * Union ops_u 只包含文件/设备操作 (互斥), 目录操作 dir_ops 独立存放。
 */

#ifndef INODE_H
#define INODE_H

#include "kernel_types.h"
#include "kernel_config.h"
#include "fs_types.h"   /* Phase F1: 公共类型 (dev_ops_t/dirent_t/vfs_stat_t/...) */

#if VFS_ENABLE

/*============================================================================
 * 内核 VFS 实现常量 (公共常量在 fs_types.h)
 *============================================================================*/

#define MAX_INODES        VFS_MAX_INODES

/*============================================================================
 * 内核 VFS 操作表类型 (只有内核 VFS 实现用)
 *============================================================================*/

struct inode;

/** 普通文件操作 (ramfs) */
typedef struct file_ops {
    kern_err_t (*open)(struct inode *inode, uint32_t flags);
    kern_err_t (*close)(struct inode *inode);
    int32_t    (*read)(struct inode *inode, void *buf, uint32_t offset, uint32_t size);
    int32_t    (*write)(struct inode *inode, const void *buf, uint32_t offset, uint32_t size);
    kern_err_t (*truncate)(struct inode *inode);  /* O_TRUNC: 清空文件内容 */
} file_ops_t;

/** 字符设备操作 (所有 CHRDEV 共享, 通过 private_data→dev_ops_t 二级分发) */
typedef struct cdev_ops {
    kern_err_t (*open)(struct inode *inode, uint32_t flags);
    kern_err_t (*close)(struct inode *inode);
    int32_t    (*read)(struct inode *inode, void *buf, uint32_t offset, uint32_t size);
    int32_t    (*write)(struct inode *inode, const void *buf, uint32_t offset, uint32_t size);
    kern_err_t (*ioctl)(struct inode *inode, uint32_t cmd, void *arg);
} cdev_ops_t;

/** 目录操作 (DIR 类型使用, 独立于 union ops_u) */
typedef struct dir_ops {
    kern_err_t (*lookup)(struct inode *dir, const char *name, struct inode **result);
    kern_err_t (*create)(struct inode *dir, const char *name, uint32_t type);
    kern_err_t (*unlink)(struct inode *dir, const char *name);
    kern_err_t (*readdir)(struct inode *dir, uint32_t index, dirent_t *entry);
} dir_ops_t;

/*============================================================================
 * inode 结构
 *============================================================================*/

typedef struct inode {
    uint32_t      ino;                       /* 唯一 inode 号 (自增) */
    char          name[INODE_NAME_LEN];      /* 文件名 */
    inode_type_t  type;
    uint32_t      flags;
    uint32_t      size;                      /* 文件大小 (FILE 类型使用) */
    uint32_t      refcount;                  /* 引用计数 */

    /* 文件/设备操作 union (互斥), 目录操作独立 */
    union {
        file_ops_t *file_ops;               /* INODE_TYPE_FILE */
        cdev_ops_t *cdev_ops;               /* INODE_TYPE_CHRDEV */
    } ops_u;

    dir_ops_t    *dir_ops;                   /* INODE_TYPE_DIR (独立字段) */

    void         *private_data;              /* FS/驱动私有数据 */

    struct inode *parent;
    struct inode *children;                  /* 子节点链表头 */
    struct inode *next_sibling;              /* 兄弟节点 */
} inode_t;

/*============================================================================
 * 挂载点
 *============================================================================*/

#define MOUNT_MAX 4

typedef struct {
    char      path[64];
    inode_t  *root_inode;
    uint32_t  root_ref_at_mount;
    uint8_t   in_use;
} mount_entry_t;

/*============================================================================
 * inode 池管理 API
 *============================================================================*/

void     inode_init(void);
inode_t *inode_alloc(inode_type_t type, const char *name);
void     inode_free(inode_t *inode);
void     inode_get(inode_t *inode);
void     inode_put(inode_t *inode);

/*============================================================================
 * inode 树操作 API
 *============================================================================*/

void     inode_add_child(inode_t *parent, inode_t *child);
inode_t *inode_lookup_child(inode_t *dir, const char *name);
kern_err_t inode_remove_child(inode_t *parent, const char *name);

#endif /* VFS_ENABLE */
#endif /* INODE_H */
