/**
 * @file devfs.h
 * @brief 设备文件系统 — 字符设备注册与 /dev 管理
 */

#ifndef DEVFS_H
#define DEVFS_H

#include "inode.h"

#if VFS_ENABLE

#if DRIVER_ENABLE
#include "dev/device.h"
#endif

void      devfs_init(inode_t *dev_dir);

#if DRIVER_ENABLE
kern_err_t devfs_register_device(const char *name, device_t *dev);
#else
kern_err_t devfs_register_device(const char *name, dev_ops_t *ops);
#endif

#endif /* VFS_ENABLE */
#endif /* DEVFS_H */
