/**
 * @file vfs.h
 * @brief VFS 核心 — 初始化、路径解析、fd 管理、文件操作 API
 */

#ifndef VFS_H
#define VFS_H

#include "kernel_types.h"
#include "inode.h"

#if VFS_ENABLE

/*============================================================================
 * VFS 核心 API
 *============================================================================*/

void     vfs_init(void);
inode_t *vfs_lookup(const char *path);

/* fd 管理 */
int      fd_alloc(tcb_t *task, inode_t *inode, uint32_t flags);
void     fd_free(tcb_t *task, int fd_index);
void     vfs_close_task_fds(tcb_t *task);

/* 文件操作 (通过 fd) */
int      vfs_open(const char *path, uint32_t flags);
kern_err_t vfs_close(int fd);
int32_t  vfs_read(int fd, void *buf, uint32_t size);
int32_t  vfs_write(int fd, const void *buf, uint32_t size);
kern_err_t vfs_ioctl(int fd, uint32_t cmd, void *arg);
int32_t  vfs_lseek(int fd, int32_t offset, int whence);
kern_err_t vfs_readdir(int fd, dirent_t *entry);
kern_err_t vfs_rewinddir(int fd);
kern_err_t vfs_unlink(const char *path);
kern_err_t vfs_mkdir(const char *path);
kern_err_t vfs_stat(const char *path, vfs_stat_t *st);

/* 挂载 */
kern_err_t vfs_mount(const char *path, inode_t *root_inode);
kern_err_t vfs_unmount(const char *path);

#endif /* VFS_ENABLE */
#endif /* VFS_H */
