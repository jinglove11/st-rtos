/**
 * @file ramfs.h
 * @brief 内存文件系统 — ramfs 文件/目录操作
 */

#ifndef RAMFS_H
#define RAMFS_H

#include "inode.h"

#if VFS_ENABLE

void      ramfs_init(inode_t *tmp_dir);
inode_t  *ramfs_create_file(inode_t *dir, const char *name);
inode_t  *ramfs_create_dir(inode_t *dir, const char *name);

#endif /* VFS_ENABLE */
#endif /* RAMFS_H */
