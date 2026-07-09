/**
 * @file fs_types.h
 * @brief 文件系统公共类型 — 与内核 VFS 实现解耦
 *
 * Phase F1: 把被外部(device.h/fs_proto.h/shell.c/test_driver.c)依赖的
 * 类型从 src/kernel/vfs/inode.h 摘出来。这些类型不依赖内核 VFS 实现,
 * 删除内核 VFS 时保留。
 *
 * 注意:dirent_t.name[INODE_NAME_LEN=32] 是 fs_server IPC 的 wire 格式
 * (fs_server.c 把 msg.path memcpy 进 entry->name),不能改大小。
 */

#ifndef FS_TYPES_H
#define FS_TYPES_H

#include "kernel_types.h"
#include <stdint.h>

/*============================================================================
 * 常量 (外部依赖)
 *============================================================================*/

#define INODE_NAME_LEN    32

/* open flags */
#define O_RDONLY  0x01
#define O_WRONLY  0x02
#define O_RDWR    0x03
#define O_CREAT   0x04
#define O_TRUNC   0x08

/* lseek whence */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/*============================================================================
 * 设备驱动操作表 (device.h 依赖)
 *============================================================================*/

/** 驱动操作表 — 具体驱动实现 (dev_ops_t 方法第一个参数是 void *priv) */
typedef struct dev_ops {
    kern_err_t (*open)(void *priv, uint32_t flags);
    kern_err_t (*close)(void *priv);
    int32_t    (*read)(void *priv, void *buf, uint32_t offset, uint32_t size);
    int32_t    (*write)(void *priv, const void *buf, uint32_t offset, uint32_t size);
    kern_err_t (*ioctl)(void *priv, uint32_t cmd, void *arg);
} dev_ops_t;

/*============================================================================
 * inode 类型枚举 (dirent_t.type / vfs_stat_t.type 用)
 *============================================================================*/

typedef enum {
    INODE_TYPE_FILE   = 0,
    INODE_TYPE_DIR    = 1,
    INODE_TYPE_CHRDEV = 2,
} inode_type_t;

/*============================================================================
 * 目录条目 + stat (fs_proto.h IPC wire 格式)
 *============================================================================*/

struct dirent {
    uint32_t    ino;
    char        name[INODE_NAME_LEN];
    uint8_t     type;          /* inode_type_t */
};
typedef struct dirent dirent_t;

typedef struct {
    uint32_t ino;
    uint32_t size;
    uint8_t  type;             /* inode_type_t */
} vfs_stat_t;

#endif /* FS_TYPES_H */
