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

#define VFS_PATH_MAX 64

/*============================================================================
 * 通用 inode-tree 目录操作
 *============================================================================*/

static kern_err_t tree_dir_lookup(inode_t *dir, const char *name, inode_t **result) {
    if (!dir || !name || !result) return KERN_ERR_PARAM;

    if (name[0] == '.' && name[1] == '\0') {
        *result = dir;
        inode_get(dir);
        return KERN_OK;
    }

    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        *result = dir->parent ? dir->parent : dir;
        inode_get(*result);
        return KERN_OK;
    }

    inode_t *child = inode_lookup_child(dir, name);
    if (!child) return KERN_ERR_NOEXIST;

    inode_get(child);
    *result = child;
    return KERN_OK;
}

static kern_err_t tree_dir_readdir(inode_t *dir, uint32_t index, dirent_t *entry) {
    if (!dir || !entry) return KERN_ERR_PARAM;

    if (index == 0) {
        entry->ino = dir->ino;
        strncpy(entry->name, ".", INODE_NAME_LEN - 1);
        entry->name[INODE_NAME_LEN - 1] = '\0';
        entry->type = (uint8_t)dir->type;
        return KERN_OK;
    }

    if (index == 1) {
        inode_t *parent = dir->parent ? dir->parent : dir;
        entry->ino = parent->ino;
        strncpy(entry->name, "..", INODE_NAME_LEN - 1);
        entry->name[INODE_NAME_LEN - 1] = '\0';
        entry->type = (uint8_t)parent->type;
        return KERN_OK;
    }

    inode_t *child = dir->children;
    uint32_t i = 2;
    while (child) {
        if (i == index) {
            entry->ino = child->ino;
            strncpy(entry->name, child->name, INODE_NAME_LEN - 1);
            entry->name[INODE_NAME_LEN - 1] = '\0';
            entry->type = (uint8_t)child->type;
            return KERN_OK;
        }
        child = child->next_sibling;
        i++;
    }

    return KERN_ERR_NOEXIST;
}

static dir_ops_t tree_dir_ops = {
    .lookup = tree_dir_lookup,
    .create = NULL,
    .unlink = NULL,
    .readdir = tree_dir_readdir,
};

/*============================================================================
 * 初始化
 *============================================================================*/

void vfs_init(void) {
    inode_init();
    memset(mount_table, 0, sizeof(mount_table));

    /* 创建根目录 / */
    root_inode = inode_alloc(INODE_TYPE_DIR, "/");
    root_inode->dir_ops = &tree_dir_ops;

    /* 创建 /dev 子目录 */
    inode_t *dev_dir = inode_alloc(INODE_TYPE_DIR, "dev");
    dev_dir->dir_ops = &tree_dir_ops;
    inode_add_child(root_inode, dev_dir);

    /* 创建 /tmp 子目录 */
    inode_t *tmp_dir = inode_alloc(INODE_TYPE_DIR, "tmp");
    tmp_dir->dir_ops = &tree_dir_ops;
    inode_add_child(root_inode, tmp_dir);

    /* 初始化 devfs (在 /dev 下注册设备) */
    devfs_init(dev_dir);

    /* 初始化 ramfs (在 /tmp 下设置文件系统) */
    ramfs_init(tmp_dir);
}

/*============================================================================
 * 路径解析
 *============================================================================*/

static kern_err_t vfs_normalize_path(const char *path, char *out, uint32_t out_size) {
    if (!path || !out || out_size < 2 || path[0] == '\0') {
        return KERN_ERR_PARAM;
    }

    out[0] = '/';
    out[1] = '\0';
    uint32_t len = 1;
    const char *p = path;

    while (*p) {
        while (*p == '/') p++;
        if (*p == '\0') break;

        char token[INODE_NAME_LEN];
        uint32_t tok_len = 0;
        while (*p && *p != '/') {
            if (tok_len >= INODE_NAME_LEN - 1) {
                return KERN_ERR_PARAM;
            }
            token[tok_len++] = *p++;
        }
        token[tok_len] = '\0';

        if (tok_len == 1 && token[0] == '.') {
            continue;
        }

        if (tok_len == 2 && token[0] == '.' && token[1] == '.') {
            if (len > 1) {
                len--;
                while (len > 1 && out[len - 1] != '/') {
                    len--;
                }
                out[len] = '\0';
            }
            continue;
        }

        uint32_t needed = len + ((len > 1) ? 1U : 0U) + tok_len + 1U;
        if (needed > out_size) {
            return KERN_ERR_PARAM;
        }

        if (len > 1) {
            out[len++] = '/';
        }
        memcpy(out + len, token, tok_len);
        len += tok_len;
        out[len] = '\0';
    }

    return KERN_OK;
}

static inode_t *vfs_mount_lookup(const char *path) {
    for (int i = 0; i < MOUNT_MAX; i++) {
        if (mount_table[i].in_use && strcmp(mount_table[i].path, path) == 0) {
            inode_get(mount_table[i].root_inode);
            return mount_table[i].root_inode;
        }
    }
    return NULL;
}

static int vfs_subtree_has_busy_descendant(inode_t *root) {
    if (!root) return 0;

    inode_t *child = root->children;
    while (child) {
        /*
         * Child inodes normally hold an allocation ref plus one tree ref.
         * A higher count means a lookup/fd/mount path still holds it.
         */
        if (child->refcount > 2) {
            return 1;
        }
        if (vfs_subtree_has_busy_descendant(child)) {
            return 1;
        }
        child = child->next_sibling;
    }

    return 0;
}

inode_t *vfs_lookup(const char *path) {
    if (!path || path[0] == '\0') return NULL;

    char norm[VFS_PATH_MAX];
    if (vfs_normalize_path(path, norm, sizeof(norm)) != KERN_OK) {
        return NULL;
    }

    /* "/" → root */
    if (strcmp(norm, "/") == 0) {
        inode_get(root_inode);
        return root_inode;
    }

    inode_t *cur = root_inode;
    inode_get(cur);

    const char *p = norm;
    while (*p == '/') p++;                    /* 跳过开头的 / */

    char token[INODE_NAME_LEN];
    char accum[VFS_PATH_MAX];
    uint32_t accum_len = 1;
    accum[0] = '/';
    accum[1] = '\0';

    while (*p) {
        int i = 0;
        while (*p && *p != '/') {
            if (i >= INODE_NAME_LEN - 1) {
                inode_put(cur);
                return NULL;
            }
            token[i++] = *p++;
        }
        token[i] = '\0';
        while (*p == '/') p++;

        if (i == 0) break;

        uint32_t needed = accum_len + ((accum_len > 1) ? 1U : 0U) +
                          (uint32_t)i + 1U;
        if (needed > sizeof(accum)) {
            inode_put(cur);
            return NULL;
        }
        if (accum_len > 1) {
            accum[accum_len++] = '/';
        }
        memcpy(accum + accum_len, token, (uint32_t)i);
        accum_len += (uint32_t)i;
        accum[accum_len] = '\0';

        if (cur->type != INODE_TYPE_DIR || !cur->dir_ops || !cur->dir_ops->lookup) {
            inode_put(cur);
            return NULL;
        }

        inode_t *child = NULL;
        if (cur->dir_ops->lookup(cur, token, &child) != KERN_OK || !child) {
            inode_put(cur);
            return NULL;
        }

        inode_t *mounted = vfs_mount_lookup(accum);
        if (mounted) {
            inode_put(child);
            child = mounted;
        }

        inode_put(cur);
        cur = child;
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

static void vfs_call_close(inode_t *inode) {
    if (!inode) return;

    switch (inode->type) {
    case INODE_TYPE_FILE:
        if (inode->ops_u.file_ops && inode->ops_u.file_ops->close) {
            inode->ops_u.file_ops->close(inode);
        }
        break;
    case INODE_TYPE_CHRDEV:
        if (inode->ops_u.cdev_ops && inode->ops_u.cdev_ops->close) {
            inode->ops_u.cdev_ops->close(inode);
        }
        break;
    case INODE_TYPE_DIR:
    default:
        break;
    }
}

void vfs_close_task_fds(tcb_t *task) {
    if (!task) return;

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!task->fd_table[i].in_use) {
            continue;
        }

        vfs_call_close(task->fd_table[i].inode);
        fd_free(task, i);
    }
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
 * 目录操作 — vfs_readdir / vfs_rewinddir
 *============================================================================*/

kern_err_t vfs_readdir(int fd, dirent_t *entry) {
#if CAP_ENABLE
    if (!entry) return KERN_ERR_PARAM;

    void *obj = cap_resolve((cap_id_t)fd, CAP_OBJ_FILE, CAP_READ);
    if (!obj) return KERN_ERR_CAP;

    int fd_index = (int)((uintptr_t)obj - 1);
    tcb_t *task = sched_get_current();
    if (!task) return KERN_ERR;

    fd_entry_t *fe = &task->fd_table[fd_index];
    if (!fe->in_use) return KERN_ERR_PARAM;

    inode_t *inode = fe->inode;
    if (inode->type != INODE_TYPE_DIR) return KERN_ERR_NOTDIR;
    if (!inode->dir_ops || !inode->dir_ops->readdir) return KERN_ERR;

    kern_err_t err = inode->dir_ops->readdir(inode, fe->offset, entry);
    if (err == KERN_OK) {
        fe->offset++;
    }
    return err;
#else
    (void)fd; (void)entry;
    return KERN_ERR;
#endif
}

kern_err_t vfs_rewinddir(int fd) {
#if CAP_ENABLE
    void *obj = cap_resolve((cap_id_t)fd, CAP_OBJ_FILE, CAP_READ);
    if (!obj) return KERN_ERR_CAP;

    int fd_index = (int)((uintptr_t)obj - 1);
    tcb_t *task = sched_get_current();
    if (!task) return KERN_ERR;

    fd_entry_t *fe = &task->fd_table[fd_index];
    if (!fe->in_use) return KERN_ERR_PARAM;
    if (fe->inode->type != INODE_TYPE_DIR) return KERN_ERR_NOTDIR;

    fe->offset = 0;
    return KERN_OK;
#else
    (void)fd;
    return KERN_ERR;
#endif
}

/*============================================================================
 * 挂载
 *============================================================================*/

kern_err_t vfs_mount(const char *path, inode_t *root) {
    if (!path || !root) return KERN_ERR_PARAM;
    if (root->type != INODE_TYPE_DIR) return KERN_ERR_NOTDIR;

    char norm[VFS_PATH_MAX];
    if (vfs_normalize_path(path, norm, sizeof(norm)) != KERN_OK) {
        return KERN_ERR_PARAM;
    }
    if (strcmp(norm, "/") == 0) {
        return KERN_ERR_PARAM;
    }

    inode_t *mount_point = vfs_lookup(norm);
    if (!mount_point) return KERN_ERR_NOEXIST;
    if (mount_point->type != INODE_TYPE_DIR) {
        inode_put(mount_point);
        return KERN_ERR_NOTDIR;
    }
    inode_put(mount_point);

    for (int i = 0; i < MOUNT_MAX; i++) {
        if (mount_table[i].in_use && strcmp(mount_table[i].path, norm) == 0) {
            return KERN_ERR_BUSY;
        }
    }

    for (int i = 0; i < MOUNT_MAX; i++) {
        if (!mount_table[i].in_use) {
            strncpy(mount_table[i].path, norm, sizeof(mount_table[i].path) - 1);
            mount_table[i].path[sizeof(mount_table[i].path) - 1] = '\0';
            mount_table[i].root_inode = root;
            inode_get(root);
            mount_table[i].root_ref_at_mount = root->refcount;
            mount_table[i].in_use = 1;
            return KERN_OK;
        }
    }
    return KERN_ERR_RESOURCE;
}

kern_err_t vfs_unmount(const char *path) {
    if (!path) return KERN_ERR_PARAM;

    char norm[VFS_PATH_MAX];
    if (vfs_normalize_path(path, norm, sizeof(norm)) != KERN_OK) {
        return KERN_ERR_PARAM;
    }

    for (int i = 0; i < MOUNT_MAX; i++) {
        if (!mount_table[i].in_use || strcmp(mount_table[i].path, norm) != 0) {
            continue;
        }

        inode_t *root = mount_table[i].root_inode;
        if (root && root->refcount > mount_table[i].root_ref_at_mount) {
            return KERN_ERR_BUSY;
        }
        if (vfs_subtree_has_busy_descendant(root)) {
            return KERN_ERR_BUSY;
        }

        if (root) {
            inode_put(root);
        }
        memset(&mount_table[i], 0, sizeof(mount_table[i]));
        return KERN_OK;
    }

    return KERN_ERR_NOEXIST;
}

#endif /* VFS_ENABLE */
