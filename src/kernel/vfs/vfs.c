/**
 * @file vfs.c
 * @brief VFS 核心实现 — 初始化、路径解析、fd 管理、文件操作
 */

#include "vfs.h"
#include "devfs.h"
#include "ramfs.h"
#include "capability.h"
#include "scheduler.h"
#include <string.h>

#if VFS_ENABLE

/* bitmap is uint32_t, max 32 inodes */
typedef char static_assert_bitmap_limit[(VFS_MAX_INODES <= 32) ? 1 : -1];

/*============================================================================
 * 全局状态
 *============================================================================*/

static inode_t *root_inode;
static mount_entry_t mount_table[MOUNT_MAX];

/*============================================================================
 * 初始化
 *============================================================================*/

void vfs_init(void) {
    inode_init();
    memset(mount_table, 0, sizeof(mount_table));

    /* 创建根目录 / */
    root_inode = inode_alloc(INODE_TYPE_DIR, "/");
    root_inode->dir_ops = NULL;  /* root uses inode tree ops directly */

    /* 创建 /dev 子目录 */
    inode_t *dev_dir = inode_alloc(INODE_TYPE_DIR, "dev");
    inode_add_child(root_inode, dev_dir);

    /* 创建 /tmp 子目录 */
    inode_t *tmp_dir = inode_alloc(INODE_TYPE_DIR, "tmp");
    inode_add_child(root_inode, tmp_dir);

    /* 初始化 devfs (在 /dev 下注册设备) */
    devfs_init(dev_dir);

    /* 初始化 ramfs (在 /tmp 下设置文件系统) */
    ramfs_init(tmp_dir);
}

/*============================================================================
 * 路径解析
 *============================================================================*/

inode_t *vfs_lookup(const char *path) {
    if (!path || path[0] == '\0') return NULL;

    /* "/" → root */
    if (strcmp(path, "/") == 0) {
        inode_get(root_inode);
        return root_inode;
    }

    inode_t *cur = root_inode;
    inode_get(cur);

    const char *p = path;
    while (*p == '/') p++;                    /* 跳过开头的 / */

    char token[INODE_NAME_LEN];

    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < INODE_NAME_LEN - 1)
            token[i++] = *p++;
        token[i] = '\0';
        while (*p == '/') p++;

        if (i == 0) break;

        inode_t *child = inode_lookup_child(cur, token);
        if (!child) {
            inode_put(cur);
            return NULL;
        }

        /* check mount_table: if child is a mount point, redirect */
        for (int m = 0; m < MOUNT_MAX; m++) {
            if (mount_table[m].in_use) {
                /* build accumulated path up to this token */
                /* simple match: child == mount root */
                if (mount_table[m].root_inode == child) {
                    inode_put(cur);
                    cur = child;
                    inode_get(cur);
                    goto next_token;
                }
            }
        }

        inode_put(cur);
        cur = child;
        inode_get(cur);
next_token:;
    }

    return cur;
}

/*============================================================================
 * fd 管理
 *============================================================================*/

int fd_alloc(tcb_t *task, inode_t *inode, uint32_t flags) {
    if (!task || !inode) return -1;

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!task->fd_table[i].in_use) {
            task->fd_table[i].inode  = inode;
            task->fd_table[i].flags  = flags;
            task->fd_table[i].offset = 0;
            task->fd_table[i].in_use = 1;
            inode_get(inode);
            return i;
        }
    }
    return -1;  /* 槽位满 */
}

void fd_free(tcb_t *task, int fd_index) {
    if (!task || fd_index < 0 || fd_index >= VFS_MAX_FDS) return;
    if (!task->fd_table[fd_index].in_use) return;

    inode_put(task->fd_table[fd_index].inode);
    memset(&task->fd_table[fd_index], 0, sizeof(fd_entry_t));
}

/*============================================================================
 * 路径拆分辅助 — 将 "/a/b/c" 拆成 parent="/a/b", name="c"
 *============================================================================*/

static int vfs_split_path(const char *path, char *parent, int parent_sz,
                           char *name, int name_sz) {
    if (!path || !parent || !name) return -1;

    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    if (!last_slash || last_slash == path) {
        /* 无斜杠或根目录 → parent="/" */
        strncpy(parent, "/", parent_sz - 1);
        parent[parent_sz - 1] = '\0';
        if (!last_slash) strncpy(name, path, name_sz - 1);
        else strncpy(name, last_slash + 1, name_sz - 1);
    } else {
        int plen = (int)(last_slash - path);
        if (plen >= parent_sz) plen = parent_sz - 1;
        memcpy(parent, path, plen);
        parent[plen] = '\0';
        strncpy(name, last_slash + 1, name_sz - 1);
    }
    name[name_sz - 1] = '\0';
    return 0;
}

/*============================================================================
 * 文件操作 — vfs_open
 *============================================================================*/

int vfs_open(const char *path, uint32_t flags) {
    /* 1. 路径解析 */
    inode_t *inode = vfs_lookup(path);

    /* O_CREAT: 文件不存在时创建 */
    if (!inode && (flags & O_CREAT)) {
        char parent_path[64];
        char file_name[INODE_NAME_LEN];
        if (vfs_split_path(path, parent_path, sizeof(parent_path),
                           file_name, INODE_NAME_LEN) < 0)
            return KERN_ERR_PARAM;
        if (file_name[0] == '\0') return KERN_ERR_PARAM;

        inode_t *parent = vfs_lookup(parent_path);
        if (!parent) return KERN_ERR_NOEXIST;

        kern_err_t cerr = KERN_ERR;
        if (parent->type == INODE_TYPE_DIR && parent->dir_ops &&
            parent->dir_ops->create) {
            cerr = parent->dir_ops->create(parent, file_name, INODE_TYPE_FILE);
        }
        inode_put(parent);

        if (cerr != KERN_OK) return cerr;
        inode = vfs_lookup(path);
    }

    if (!inode) return KERN_ERR_NOEXIST;

    /* 2. 分配 fd */
    tcb_t *task = sched_get_current();
    int fd_index = fd_alloc(task, inode, flags);
    if (fd_index < 0) {
        inode_put(inode);
        return KERN_ERR_RESOURCE;
    }

    /* 3. 调用 ops->open (inode 仍有效, fd_alloc 持有引用) */
    kern_err_t err = KERN_OK;
    switch (inode->type) {
    case INODE_TYPE_FILE:
        if (inode->ops_u.file_ops && inode->ops_u.file_ops->open)
            err = inode->ops_u.file_ops->open(inode, flags);
        break;
    case INODE_TYPE_CHRDEV:
        if (inode->ops_u.cdev_ops && inode->ops_u.cdev_ops->open)
            err = inode->ops_u.cdev_ops->open(inode, flags);
        break;
    case INODE_TYPE_DIR:
        /* 目录 open 成功 (允许 readdir 等后续操作) */
        break;
    }

    /* O_TRUNC: 截断文件 */
    if ((flags & O_TRUNC) && inode->type == INODE_TYPE_FILE &&
        inode->ops_u.file_ops && inode->ops_u.file_ops->truncate)
        inode->ops_u.file_ops->truncate(inode);

    /* ops->open 完成后释放查找引用 (fd_alloc 持有独立引用) */
    inode_put(inode);

    if (err != KERN_OK) {
        fd_free(task, fd_index);
        return err;
    }

    /* 4. 创建能力令牌 (按 open flags 映射权限) */
    uint32_t cap_rights = CAP_MANAGE;
    if (flags & O_RDONLY) cap_rights |= CAP_READ;
    if (flags & O_WRONLY) cap_rights |= CAP_WRITE;

    cap_id_t cap = cap_create((void *)(uintptr_t)(fd_index + 1),
                              CAP_OBJ_FILE,
                              cap_rights,
                              (uint8_t)(task ? task->id : 0));
    if (cap < 0) {
        fd_free(task, fd_index);
        return KERN_ERR_RESOURCE;
    }

    return (int)cap;
}

/*============================================================================
 * 文件操作 — vfs_close
 *============================================================================*/

kern_err_t vfs_close(int fd) {
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)fd, CAP_OBJ_FILE, CAP_MANAGE);
    if (!obj) return KERN_ERR_CAP;

    int fd_index = (int)((uintptr_t)obj - 1);
    tcb_t *task = sched_get_current();
    if (!task) return KERN_ERR;

    fd_entry_t *fe = &task->fd_table[fd_index];
    if (!fe->in_use) return KERN_ERR_PARAM;

    inode_t *inode = fe->inode;

    /* 调用 close */
    switch (inode->type) {
    case INODE_TYPE_FILE:
        if (inode->ops_u.file_ops && inode->ops_u.file_ops->close)
            inode->ops_u.file_ops->close(inode);
        break;
    case INODE_TYPE_CHRDEV:
        if (inode->ops_u.cdev_ops && inode->ops_u.cdev_ops->close)
            inode->ops_u.cdev_ops->close(inode);
        break;
    case INODE_TYPE_DIR:
        break;
    }

    fd_free(task, fd_index);
    cap_delete((cap_id_t)fd);
    return KERN_OK;
#else
    return KERN_ERR;
#endif
}

/*============================================================================
 * 文件操作 — vfs_read
 *============================================================================*/

int32_t vfs_read(int fd, void *buf, uint32_t size) {
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)fd, CAP_OBJ_FILE, CAP_READ);
    if (!obj) return KERN_ERR_CAP;

    int fd_index = (int)((uintptr_t)obj - 1);
    tcb_t *task = sched_get_current();
    if (!task) return KERN_ERR;

    fd_entry_t *fe = &task->fd_table[fd_index];
    if (!fe->in_use) return KERN_ERR_PARAM;

    inode_t *inode = fe->inode;
    int32_t result;

    switch (inode->type) {
    case INODE_TYPE_FILE:
        if (inode->ops_u.file_ops && inode->ops_u.file_ops->read)
            result = inode->ops_u.file_ops->read(inode, buf, fe->offset, size);
        else
            result = KERN_ERR;
        break;
    case INODE_TYPE_CHRDEV:
        if (inode->ops_u.cdev_ops && inode->ops_u.cdev_ops->read)
            result = inode->ops_u.cdev_ops->read(inode, buf, fe->offset, size);
        else
            result = KERN_ERR;
        break;
    case INODE_TYPE_DIR:
    default:
        result = KERN_ERR_ISDIR;
        break;
    }

    if (result > 0) fe->offset += (uint32_t)result;
    return result;
#else
    (void)buf; (void)size;
    return KERN_ERR;
#endif
}

/*============================================================================
 * 文件操作 — vfs_write
 *============================================================================*/

int32_t vfs_write(int fd, const void *buf, uint32_t size) {
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)fd, CAP_OBJ_FILE, CAP_WRITE);
    if (!obj) return KERN_ERR_CAP;

    int fd_index = (int)((uintptr_t)obj - 1);
    tcb_t *task = sched_get_current();
    if (!task) return KERN_ERR;

    fd_entry_t *fe = &task->fd_table[fd_index];
    if (!fe->in_use) return KERN_ERR_PARAM;

    inode_t *inode = fe->inode;
    int32_t result;

    switch (inode->type) {
    case INODE_TYPE_FILE:
        if (inode->ops_u.file_ops && inode->ops_u.file_ops->write)
            result = inode->ops_u.file_ops->write(inode, buf, fe->offset, size);
        else
            result = KERN_ERR;
        break;
    case INODE_TYPE_CHRDEV:
        if (inode->ops_u.cdev_ops && inode->ops_u.cdev_ops->write)
            result = inode->ops_u.cdev_ops->write(inode, buf, fe->offset, size);
        else
            result = KERN_ERR;
        break;
    case INODE_TYPE_DIR:
    default:
        result = KERN_ERR_ISDIR;
        break;
    }

    if (result > 0) fe->offset += (uint32_t)result;
    return result;
#else
    (void)buf; (void)size;
    return KERN_ERR;
#endif
}

/*============================================================================
 * 文件操作 — vfs_ioctl
 *============================================================================*/

kern_err_t vfs_ioctl(int fd, uint32_t cmd, void *arg) {
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)fd, CAP_OBJ_FILE, CAP_READ);
    if (!obj) return KERN_ERR_CAP;

    int fd_index = (int)((uintptr_t)obj - 1);
    tcb_t *task = sched_get_current();
    if (!task) return KERN_ERR;

    fd_entry_t *fe = &task->fd_table[fd_index];
    if (!fe->in_use) return KERN_ERR_PARAM;

    inode_t *inode = fe->inode;

    switch (inode->type) {
    case INODE_TYPE_CHRDEV:
        if (inode->ops_u.cdev_ops && inode->ops_u.cdev_ops->ioctl)
            return inode->ops_u.cdev_ops->ioctl(inode, cmd, arg);
        return KERN_ERR;
    case INODE_TYPE_FILE:
        return KERN_ERR;  /* 普通文件不支持 ioctl */
    default:
        return KERN_ERR;
    }
#else
    (void)cmd; (void)arg;
    return KERN_ERR;
#endif
}

/*============================================================================
 * 文件操作 — vfs_lseek
 *============================================================================*/

int32_t vfs_lseek(int fd, int32_t offset, int whence) {
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)fd, CAP_OBJ_FILE, CAP_READ);
    if (!obj) return KERN_ERR_CAP;

    int fd_index = (int)((uintptr_t)obj - 1);
    tcb_t *task = sched_get_current();
    if (!task) return KERN_ERR;

    fd_entry_t *fe = &task->fd_table[fd_index];
    if (!fe->in_use) return KERN_ERR_PARAM;

    inode_t *inode = fe->inode;

    switch (whence) {
    case SEEK_SET:
        if (offset < 0) return KERN_ERR_PARAM;
        fe->offset = (uint32_t)offset;
        break;
    case SEEK_CUR: {
        int32_t new_off = (int32_t)fe->offset + offset;
        if (new_off < 0) return KERN_ERR_PARAM;
        fe->offset = (uint32_t)new_off;
        break;
    }
    case SEEK_END: {
        int32_t new_off = (int32_t)inode->size + offset;
        if (new_off < 0) return KERN_ERR_PARAM;
        fe->offset = (uint32_t)new_off;
        break;
    }
    default:
        return KERN_ERR_PARAM;
    }

    return (int32_t)fe->offset;
#else
    (void)offset; (void)whence;
    return KERN_ERR;
#endif
}

/*============================================================================
 * 挂载
 *============================================================================*/

kern_err_t vfs_mount(const char *path, inode_t *root) {
    if (!path || !root) return KERN_ERR_PARAM;

    for (int i = 0; i < MOUNT_MAX; i++) {
        if (!mount_table[i].in_use) {
            strncpy(mount_table[i].path, path, sizeof(mount_table[i].path) - 1);
            mount_table[i].root_inode = root;
            mount_table[i].in_use = 1;
            return KERN_OK;
        }
    }
    return KERN_ERR_RESOURCE;
}

#endif /* VFS_ENABLE */
