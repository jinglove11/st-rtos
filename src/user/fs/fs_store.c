/**
 * @file fs_store.c
 * @brief fs_server 内部 FS 存储 — inode 池 + ramfs (Phase B, context-based)
 *
 * 移植自内核 inode.c + ramfs.c + vfs.c,简化适配 user 态:
 *   - 所有状态在 fs_store_ctx_t 里 (放在 memblock,经 sys_mem_map 映射)
 *   - bump allocator 在 memblock 上分配 (32 字节对齐)
 *   - 类型直接分派,不用函数指针表
 *   - 单线程 (fs_server 服务循环),无需锁
 */

#include "fs_store.h"
#include <stdint.h>

#if VFS_ENABLE && CAP_ENABLE

/*============================================================================
 * memblock 上的 bump allocator
 *============================================================================*/

static void *fs_store_alloc(fs_store_ctx_t *ctx, uint32_t size) {
    if (ctx == NULL || ctx->store_base == NULL) {
        return NULL;
    }
    uint32_t aligned = (size + 31U) & ~31U;
    if (ctx->store_off + aligned > ctx->store_size) {
        return NULL;
    }
    void *p = ctx->store_base + ctx->store_off;
    ctx->store_off += aligned;
    for (uint32_t i = 0; i < aligned; i++) {
        ((uint8_t *)p)[i] = 0;
    }
    return p;
}

/*============================================================================
 * inode 池
 *============================================================================*/

static fs_inode_t *fs_inode_find_free(fs_store_ctx_t *ctx) {
    for (int i = 0; i < FS_STORE_MAX_INODES; i++) {
        if (ctx->inode_pool[i].ino == 0) {
            return &ctx->inode_pool[i];
        }
    }
    return NULL;
}

static fs_inode_t *fs_inode_alloc(fs_store_ctx_t *ctx, uint8_t type,
                                  const char *name) {
    fs_inode_t *inode = fs_inode_find_free(ctx);
    if (inode == NULL) return NULL;
    for (uint32_t i = 0; i < sizeof(fs_inode_t); i++) {
        ((uint8_t *)inode)[i] = 0;
    }
    inode->ino = ctx->ino_counter++;
    inode->type = type;
    inode->refcount = 1;
    for (uint32_t i = 0; i < FS_NAME_LEN - 1; i++) {
        inode->name[i] = name[i];
        if (name[i] == '\0') break;
    }
    inode->name[FS_NAME_LEN - 1] = '\0';
    return inode;
}

static void fs_inode_get(fs_inode_t *inode) {
    if (inode) inode->refcount++;
}

static void fs_inode_put(fs_inode_t *inode) {
    if (inode == NULL) return;
    if (inode->refcount > 0) {
        inode->refcount--;
        if (inode->refcount == 0) {
            inode->ino = 0;
        }
    }
}

static void fs_inode_add_child(fs_inode_t *parent, fs_inode_t *child) {
    if (parent == NULL || child == NULL) return;
    child->parent = parent;
    child->next_sibling = NULL;
    if (parent->children == NULL) {
        parent->children = child;
    } else {
        fs_inode_t *sib = parent->children;
        while (sib->next_sibling) sib = sib->next_sibling;
        sib->next_sibling = child;
    }
    fs_inode_get(child);
}

static fs_inode_t *fs_inode_lookup_child(fs_inode_t *dir, const char *name) {
    if (dir == NULL || name == NULL) return NULL;
    fs_inode_t *child = dir->children;
    while (child) {
        int eq = 1;
        for (uint32_t i = 0; i < FS_NAME_LEN; i++) {
            if (child->name[i] != name[i]) { eq = 0; break; }
            if (name[i] == '\0') break;
        }
        if (eq) return child;
        child = child->next_sibling;
    }
    return NULL;
}

/*============================================================================
 * ramfs 文件数据
 *============================================================================*/

#define RAMFS_BLOCK_SIZE 64

static int fs_ramfs_grow(fs_store_ctx_t *ctx, fs_inode_t *inode, uint32_t needed) {
    uint32_t new_cap = inode->data_cap;
    if (new_cap == 0) new_cap = RAMFS_BLOCK_SIZE;
    while (new_cap < needed) new_cap += RAMFS_BLOCK_SIZE;
    uint8_t *new_buf = fs_store_alloc(ctx, new_cap);
    if (new_buf == NULL) return -1;
    if (inode->data_buf && inode->size > 0) {
        for (uint32_t i = 0; i < inode->size; i++) {
            new_buf[i] = inode->data_buf[i];
        }
    }
    inode->data_buf = new_buf;
    inode->data_cap = new_cap;
    return 0;
}

static int32_t fs_ramfs_read(fs_inode_t *inode, void *buf,
                             uint32_t offset, uint32_t size) {
    if (inode->data_buf == NULL) return 0;
    if (offset >= inode->size) return 0;
    uint32_t avail = inode->size - offset;
    if (size > avail) size = avail;
    uint8_t *dst = (uint8_t *)buf;
    uint8_t *src = inode->data_buf + offset;
    for (uint32_t i = 0; i < size; i++) dst[i] = src[i];
    return (int32_t)size;
}

static int32_t fs_ramfs_write(fs_store_ctx_t *ctx, fs_inode_t *inode,
                              const void *buf, uint32_t offset, uint32_t size) {
    uint32_t needed = offset + size;
    if (needed > inode->data_cap) {
        if (fs_ramfs_grow(ctx, inode, needed) < 0) return -4;
    }
    uint8_t *dst = inode->data_buf + offset;
    const uint8_t *src = (const uint8_t *)buf;
    for (uint32_t i = 0; i < size; i++) dst[i] = src[i];
    if (needed > inode->size) inode->size = needed;
    return (int32_t)size;
}

/*============================================================================
 * 目录操作
 *============================================================================*/

static int fs_dir_create(fs_store_ctx_t *ctx, fs_inode_t *dir,
                         const char *name, uint8_t type) {
    if (dir == NULL || name == NULL) return -2;
    if (fs_inode_lookup_child(dir, name) != NULL) return -5;
    fs_inode_t *child = fs_inode_alloc(ctx, type, name);
    if (child == NULL) return -4;
    fs_inode_add_child(dir, child);
    return 0;
}

static int fs_dir_readdir(fs_inode_t *dir, uint32_t index, fs_dirent_t *entry) {
    if (dir == NULL || entry == NULL) return -2;
    if (index == 0) {
        entry->ino = dir->ino;
        entry->name[0] = '.'; entry->name[1] = '\0';
        entry->type = dir->type;
        return 0;
    }
    if (index == 1) {
        fs_inode_t *parent = dir->parent ? dir->parent : dir;
        entry->ino = parent->ino;
        entry->name[0] = '.'; entry->name[1] = '.'; entry->name[2] = '\0';
        entry->type = parent->type;
        return 0;
    }
    fs_inode_t *child = dir->children;
    uint32_t i = 2;
    while (child) {
        if (i == index) {
            entry->ino = child->ino;
            for (uint32_t j = 0; j < FS_NAME_LEN; j++) {
                entry->name[j] = child->name[j];
                if (child->name[j] == '\0') break;
            }
            entry->type = child->type;
            return 0;
        }
        child = child->next_sibling;
        i++;
    }
    return -5;
}

/*============================================================================
 * 路径解析
 *============================================================================*/

static fs_inode_t *fs_path_resolve(fs_store_ctx_t *ctx, const char *path,
                                   fs_inode_t **parent_out,
                                   char *name_out, uint32_t name_max) {
    if (path == NULL || path[0] != '/' || ctx == NULL || ctx->root == NULL) {
        return NULL;
    }
    fs_inode_t *cur = ctx->root;
    fs_inode_get(cur);

    uint32_t i = 1;
    while (path[i] != '\0') {
        char token[FS_NAME_LEN];
        uint32_t tlen = 0;
        while (path[i] != '\0' && path[i] != '/' && tlen < FS_NAME_LEN - 1) {
            token[tlen++] = path[i++];
        }
        token[tlen] = '\0';
        while (path[i] == '/') i++;

        if (tlen == 0) continue;

        if (parent_out) {
            if (*parent_out) fs_inode_put(*parent_out);
            *parent_out = cur;
            fs_inode_get(*parent_out);
        }
        if (name_out) {
            for (uint32_t j = 0; j < name_max; j++) {
                name_out[j] = token[j];
                if (token[j] == '\0') break;
            }
        }

        fs_inode_t *next = NULL;
        if (token[0] == '.' && token[1] == '\0') {
            next = cur;
            fs_inode_get(next);
        } else if (token[0] == '.' && token[1] == '.' && token[2] == '\0') {
            next = cur->parent ? cur->parent : cur;
            fs_inode_get(next);
        } else {
            next = fs_inode_lookup_child(cur, token);
            if (next == NULL) {
                /* lookup 失败:不清 parent_out (调用方如 mkdir/open O_CREAT
                 * 需要知道父目录在哪)。只释放 cur,返回 NULL。 */
                fs_inode_put(cur);
                return NULL;
            }
            fs_inode_get(next);
        }
        fs_inode_put(cur);
        cur = next;
    }
    return cur;
}

fs_inode_t *fs_store_lookup(fs_store_ctx_t *ctx, const char *path) {
    return fs_path_resolve(ctx, path, NULL, NULL, 0);
}

void fs_store_put(fs_store_ctx_t *ctx, fs_inode_t *inode) {
    (void)ctx;
    fs_inode_put(inode);
}

/*============================================================================
 * fd 表
 *============================================================================*/

static fs_fd_t *fs_fd_get(fs_store_ctx_t *ctx, int fd) {
    /* fd 是 1-based (0=无效,1..FS_STORE_MAX_FDS) */
    if (fd <= 0 || fd > FS_STORE_MAX_FDS) return NULL;
    int idx = fd - 1;
    if (!ctx->fds[idx].in_use) return NULL;
    return &ctx->fds[idx];
}

static int fs_fd_alloc(fs_store_ctx_t *ctx, fs_inode_t *inode) {
    for (int i = 0; i < FS_STORE_MAX_FDS; i++) {
        if (!ctx->fds[i].in_use) {
            ctx->fds[i].in_use = 1;
            ctx->fds[i].inode = inode;
            ctx->fds[i].offset = 0;
            fs_inode_get(inode);
            return i + 1;  /* 1-based token */
        }
    }
    return -4;
}

static void fs_fd_free(fs_store_ctx_t *ctx, int fd) {
    if (fd <= 0 || fd > FS_STORE_MAX_FDS) return;
    int idx = fd - 1;
    if (ctx->fds[idx].in_use) {
        fs_inode_put(ctx->fds[idx].inode);
        ctx->fds[idx].in_use = 0;
        ctx->fds[idx].inode = NULL;
        ctx->fds[idx].offset = 0;
    }
}

/*============================================================================
 * 文件操作 API
 *============================================================================*/

int fs_store_open(fs_store_ctx_t *ctx, const char *path, uint32_t flags) {
    fs_inode_t *inode = fs_store_lookup(ctx, path);
    if (inode == NULL) {
        if (flags & FS_O_CREAT) {
            fs_inode_t *parent = NULL;
            char name[FS_NAME_LEN];
            inode = fs_path_resolve(ctx, path, &parent, name, FS_NAME_LEN);
            if (parent == NULL) return -9;
            int err = fs_dir_create(ctx, parent, name, FS_INODE_FILE);
            fs_inode_put(parent);
            if (err != 0) return err;
            inode = fs_store_lookup(ctx, path);
            if (inode == NULL) return -9;
        } else {
            return -9;
        }
    }

    /* 目录可以 open (用于 readdir),但不能 O_TRUNC/O_CREAT 已有目录 */
    if ((flags & FS_O_TRUNC) && inode->type == FS_INODE_FILE) {
        inode->size = 0;
    }

    int fd = fs_fd_alloc(ctx, inode);
    fs_inode_put(inode);
    return fd;
}

int fs_store_close(fs_store_ctx_t *ctx, int fd) {
    fs_fd_t *f = fs_fd_get(ctx, fd);
    if (f == NULL) return -2;
    fs_fd_free(ctx, fd);
    return 0;
}

int32_t fs_store_read(fs_store_ctx_t *ctx, int fd, void *buf, uint32_t len) {
    fs_fd_t *f = fs_fd_get(ctx, fd);
    if (f == NULL) return -2;
    if (f->inode->type == FS_INODE_DIR) return -14;  /* ISDIR */

    if (f->inode->type == FS_INODE_FILE) {
        int32_t n = fs_ramfs_read(f->inode, buf, f->offset, len);
        if (n > 0) f->offset += (uint32_t)n;
        return n;
    }
    return -16;  /* CHRDEV:由 fs_server_run 转发,store 不处理 */
}

int32_t fs_store_write(fs_store_ctx_t *ctx, int fd, const void *buf, uint32_t len) {
    fs_fd_t *f = fs_fd_get(ctx, fd);
    if (f == NULL) return -2;
    if (f->inode->type == FS_INODE_DIR) return -14;

    if (f->inode->type == FS_INODE_FILE) {
        int32_t n = fs_ramfs_write(ctx, f->inode, buf, f->offset, len);
        if (n > 0) f->offset += (uint32_t)n;
        return n;
    }
    return -16;
}

int fs_store_lseek(fs_store_ctx_t *ctx, int fd, int32_t offset, uint32_t whence) {
    fs_fd_t *f = fs_fd_get(ctx, fd);
    if (f == NULL) return -2;
    int32_t newpos = 0;
    if (whence == FS_SEEK_SET) newpos = offset;
    else if (whence == FS_SEEK_CUR) newpos = (int32_t)f->offset + offset;
    else if (whence == FS_SEEK_END) newpos = (int32_t)f->inode->size + offset;
    else return -2;
    if (newpos < 0) return -2;
    f->offset = (uint32_t)newpos;
    return (int)f->offset;
}

int fs_store_readdir(fs_store_ctx_t *ctx, int fd, fs_dirent_t *entry) {
    fs_fd_t *f = fs_fd_get(ctx, fd);
    if (f == NULL) return -2;
    if (f->inode->type != FS_INODE_DIR) return -13;
    int err = fs_dir_readdir(f->inode, f->offset, entry);
    if (err == 0) f->offset++;
    return err;
}

int fs_store_mkdir(fs_store_ctx_t *ctx, const char *path) {
    fs_inode_t *parent = NULL;
    char name[FS_NAME_LEN];
    fs_inode_t *inode = fs_path_resolve(ctx, path, &parent, name, FS_NAME_LEN);
    if (inode != NULL) {
        fs_inode_put(inode);
        if (parent) fs_inode_put(parent);
        return -8;
    }
    if (parent == NULL) return -9;
    int err = fs_dir_create(ctx, parent, name, FS_INODE_DIR);
    fs_inode_put(parent);
    return err;
}

int fs_store_unlink(fs_store_ctx_t *ctx, const char *path) {
    fs_inode_t *inode = fs_store_lookup(ctx, path);
    if (inode == NULL) return -9;
    if (inode->parent == NULL) {
        fs_inode_put(inode);
        return -12;
    }
    fs_inode_t *parent = inode->parent;
    fs_inode_t *prev = NULL;
    fs_inode_t *c = parent->children;
    while (c) {
        if (c == inode) {
            if (prev) prev->next_sibling = c->next_sibling;
            else parent->children = c->next_sibling;
            break;
        }
        prev = c;
        c = c->next_sibling;
    }
    fs_inode_put(inode);
    fs_inode_put(inode);
    return 0;
}

int fs_store_stat(fs_store_ctx_t *ctx, const char *path, fs_statinfo_t *st) {
    fs_inode_t *inode = fs_store_lookup(ctx, path);
    if (inode == NULL) return -9;
    st->ino = inode->ino;
    st->size = inode->size;
    st->type = inode->type;
    fs_inode_put(inode);
    return 0;
}

/*============================================================================
 * devfs
 *============================================================================*/

int fs_store_register_dev(fs_store_ctx_t *ctx, const char *name, int dev_ep_cap) {
    if (ctx == NULL || ctx->dev_dir == NULL || name == NULL) return -2;
    if (fs_inode_lookup_child(ctx->dev_dir, name) != NULL) return -8;
    fs_inode_t *node = fs_inode_alloc(ctx, FS_INODE_CHRDEV, name);
    if (node == NULL) return -4;
    node->dev_ep_cap = dev_ep_cap;
    fs_inode_add_child(ctx->dev_dir, node);
    return 0;
}

/*============================================================================
 * 初始化:memblock 布局 [fs_store_ctx_t][inode 池][ramfs 数据区...]
 *============================================================================*/
fs_store_ctx_t *fs_store_init(void *store_buf, uint32_t store_size) {
    if (store_buf == NULL || store_size < sizeof(fs_store_ctx_t)) {
        return NULL;
    }
    fs_store_ctx_t *ctx = (fs_store_ctx_t *)store_buf;
    for (uint32_t i = 0; i < sizeof(fs_store_ctx_t); i++) {
        ((uint8_t *)ctx)[i] = 0;
    }
    ctx->store_base = (uint8_t *)store_buf;
    ctx->store_size = store_size;
    ctx->store_off = (uint32_t)sizeof(fs_store_ctx_t);
    ctx->store_off = (ctx->store_off + 31U) & ~31U;  /* 32 对齐 */
    ctx->ino_counter = 1;

    ctx->inode_pool = (fs_inode_t *)fs_store_alloc(ctx,
        FS_STORE_MAX_INODES * (uint32_t)sizeof(fs_inode_t));
    if (ctx->inode_pool == NULL) return NULL;

    ctx->root = fs_inode_alloc(ctx, FS_INODE_DIR, "");
    ctx->tmp_dir = fs_inode_alloc(ctx, FS_INODE_DIR, "tmp");
    ctx->dev_dir = fs_inode_alloc(ctx, FS_INODE_DIR, "dev");
    if (ctx->root == NULL || ctx->tmp_dir == NULL || ctx->dev_dir == NULL) {
        return NULL;
    }
    fs_inode_add_child(ctx->root, ctx->tmp_dir);
    fs_inode_add_child(ctx->root, ctx->dev_dir);
    return ctx;
}

#endif /* VFS_ENABLE && CAP_ENABLE */
