/**
 * @file fs_store.c
 * @brief fs_server 内部 FS 存储实现 — inode 池 + ramfs (Phase B)
 *
 * 移植自内核 inode.c + ramfs.c + vfs.c 的逻辑,简化适配 user 态:
 *   - inode 池在 sys_mem_alloc 的 memblock 里 (bump allocate)
 *   - ramfs 文件数据也在同一 memblock 里
 *   - 不依赖内核 kmalloc,用自己的 fs_store_alloc
 *   - ops 表用类型直接分派 (不用函数指针表,简化)
 */

#include "fs_store.h"
#include "user_api.h"
#include <stdint.h>

#if VFS_ENABLE && CAP_ENABLE

/*============================================================================
 * memblock 上的 bump allocator
 *============================================================================*/

static uint8_t *g_store_base = NULL;
static uint32_t g_store_size = 0;
static uint32_t g_store_off  = 0;   /* 下一个可分配偏移 */

/* 32 字节对齐分配 (满足 MPU region 对齐要求) */
static void *fs_store_alloc(uint32_t size) {
    if (g_store_base == NULL) {
        return NULL;
    }
    /* 向上对齐到 32 字节 */
    uint32_t aligned = (size + 31U) & ~31U;
    if (g_store_off + aligned > g_store_size) {
        return NULL;
    }
    void *p = g_store_base + g_store_off;
    g_store_off += aligned;
    /* 清零 */
    for (uint32_t i = 0; i < aligned; i++) {
        ((uint8_t *)p)[i] = 0;
    }
    return p;
}

/*============================================================================
 * inode 池
 *============================================================================*/

static fs_inode_t *g_inode_pool = NULL;
static uint32_t g_ino_counter = 1;

static fs_inode_t *fs_inode_find_free(void) {
    for (int i = 0; i < FS_STORE_MAX_INODES; i++) {
        if (g_inode_pool[i].ino == 0) {
            return &g_inode_pool[i];
        }
    }
    return NULL;
}

static fs_inode_t *fs_inode_alloc(uint8_t type, const char *name) {
    fs_inode_t *inode = fs_inode_find_free();
    if (inode == NULL) {
        return NULL;
    }
    /* 清零 */
    for (uint32_t i = 0; i < sizeof(fs_inode_t); i++) {
        ((uint8_t *)inode)[i] = 0;
    }
    inode->ino = g_ino_counter++;
    inode->type = type;
    inode->refcount = 1;
    /* 拷贝名字 */
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
        /* refcount=0 时清回池 (ramfs 数据留在 data_buf,下次 alloc 清零) */
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
        while (sib->next_sibling) {
            sib = sib->next_sibling;
        }
        sib->next_sibling = child;
    }
    fs_inode_get(child);
}

static fs_inode_t *fs_inode_lookup_child(fs_inode_t *dir, const char *name) {
    if (dir == NULL || name == NULL) return NULL;
    fs_inode_t *child = dir->children;
    while (child) {
        /* 字符串比较 */
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
 * ramfs 文件数据操作
 *============================================================================*/

#define RAMFS_BLOCK_SIZE 64

/* 扩展文件数据缓冲 (在 store 里重新分配 + 拷贝) */
static int fs_ramfs_grow(fs_inode_t *inode, uint32_t needed) {
    uint32_t new_cap = inode->data_cap;
    if (new_cap == 0) new_cap = RAMFS_BLOCK_SIZE;
    while (new_cap < needed) {
        new_cap += RAMFS_BLOCK_SIZE;
    }
    uint8_t *new_buf = fs_store_alloc(new_cap);
    if (new_buf == NULL) return -1;
    /* 拷贝旧数据 */
    if (inode->data_buf && inode->size > 0) {
        for (uint32_t i = 0; i < inode->size; i++) {
            new_buf[i] = inode->data_buf[i];
        }
    }
    /* 旧 buf 在 store 里,不 free (bump allocator 不回收) */
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
    for (uint32_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }
    return (int32_t)size;
}

static int32_t fs_ramfs_write(fs_inode_t *inode, const void *buf,
                           uint32_t offset, uint32_t size) {
    uint32_t needed = offset + size;
    if (needed > inode->data_cap) {
        if (fs_ramfs_grow(inode, needed) < 0) return -4;  /* KERN_ERR_RESOURCE */
    }
    uint8_t *dst = inode->data_buf + offset;
    const uint8_t *src = (const uint8_t *)buf;
    for (uint32_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }
    if (needed > inode->size) inode->size = needed;
    return (int32_t)size;
}

/*============================================================================
 * 目录操作
 *============================================================================*/

static int fs_dir_create(fs_inode_t *dir, const char *name, uint8_t type) {
    if (dir == NULL || name == NULL) return -2;  /* PARAM */
    if (fs_inode_lookup_child(dir, name) != NULL) return -5;  /* STATE:已存在 */

    fs_inode_t *child = fs_inode_alloc(type, name);
    if (child == NULL) return -4;  /* RESOURCE */
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
    return -5;  /* 越界 */
}

/*============================================================================
 * 根目录树
 *============================================================================*/

static fs_inode_t *g_root = NULL;     /* "/" */
static fs_inode_t *g_tmp = NULL;      /* "/tmp" (ramfs) */
static fs_inode_t *g_dev = NULL;      /* "/dev" (devfs) */

int fs_store_init(void *store_buf, uint32_t store_size) {
    if (store_buf == NULL || store_size == 0) {
        return -2;
    }
    g_store_base = (uint8_t *)store_buf;
    g_store_size = store_size;
    g_store_off = 0;

    /* 分配 inode 池 (数组) */
    g_inode_pool = (fs_inode_t *)fs_store_alloc(
        FS_STORE_MAX_INODES * (uint32_t)sizeof(fs_inode_t));
    if (g_inode_pool == NULL) {
        return -4;
    }
    g_ino_counter = 1;

    /* 构造 / /tmp /dev */
    g_root = fs_inode_alloc(FS_INODE_DIR, "");
    g_tmp = fs_inode_alloc(FS_INODE_DIR, "tmp");
    g_dev = fs_inode_alloc(FS_INODE_DIR, "dev");
    if (g_root == NULL || g_tmp == NULL || g_dev == NULL) {
        return -4;
    }
    fs_inode_add_child(g_root, g_tmp);
    fs_inode_add_child(g_root, g_dev);
    return 0;
}

/*============================================================================
 * 路径解析
 *============================================================================*/

/* 解析路径,返回目标 inode (带 ref+1),parent_out 返回父目录 (带 ref+1)。
 * name_out 返回最后一段名字 (指向 buf)。 */
static fs_inode_t *fs_path_resolve(const char *path,
                                fs_inode_t **parent_out,
                                char *name_out, uint32_t name_max) {
    if (path == NULL || path[0] != '/') {
        return NULL;
    }
    fs_inode_t *cur = g_root;
    fs_inode_get(cur);

    uint32_t i = 1;  /* 跳过开头的 / */
    while (path[i] != '\0') {
        /* 提取下一段 token */
        char token[FS_NAME_LEN];
        uint32_t tlen = 0;
        while (path[i] != '\0' && path[i] != '/' && tlen < FS_NAME_LEN - 1) {
            token[tlen++] = path[i++];
        }
        token[tlen] = '\0';
        while (path[i] == '/') i++;  /* 跳过多余 / */

        if (tlen == 0) continue;  /* 空段 */

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

        /* 找子节点 (支持 . 和 ..) */
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
                fs_inode_put(cur);
                if (parent_out && *parent_out) { fs_inode_put(*parent_out); *parent_out = NULL; }
                return NULL;  /* 不存在 */
            }
            fs_inode_get(next);
        }
        fs_inode_put(cur);
        cur = next;
    }
    return cur;
}

fs_inode_t *fs_store_lookup(const char *path) {
    return fs_path_resolve(path, NULL, NULL, 0);
}

void fs_store_put(fs_inode_t *inode) {
    fs_inode_put(inode);
}

/*============================================================================
 * fd 表 (fs_server 进程内,服务所有客户端)
 *============================================================================*/

typedef struct {
    uint8_t   in_use;
    fs_inode_t *inode;
    uint32_t  offset;
} fs_fd_t;

static fs_fd_t g_fds[FS_STORE_MAX_FDS];

static int fs_fd_alloc(fs_inode_t *inode) {
    for (int i = 0; i < FS_STORE_MAX_FDS; i++) {
        if (!g_fds[i].in_use) {
            g_fds[i].in_use = 1;
            g_fds[i].inode = inode;
            g_fds[i].offset = 0;
            fs_inode_get(inode);
            return i;
        }
    }
    return -4;  /* RESOURCE */
}

static fs_fd_t *fs_fd_get(int fd) {
    if (fd < 0 || fd >= FS_STORE_MAX_FDS) return NULL;
    if (!g_fds[fd].in_use) return NULL;
    return &g_fds[fd];
}

static void fs_fd_free(int fd) {
    if (fd < 0 || fd >= FS_STORE_MAX_FDS) return;
    if (g_fds[fd].in_use) {
        fs_inode_put(g_fds[fd].inode);
        g_fds[fd].in_use = 0;
        g_fds[fd].inode = NULL;
        g_fds[fd].offset = 0;
    }
}

/*============================================================================
 * 文件操作 API
 *============================================================================*/

int fs_store_open(const char *path, uint32_t flags) {
    fs_inode_t *inode = fs_store_lookup(path);
    if (inode == NULL) {
        /* O_CREAT:尝试创建 */
        if (flags & FS_O_CREAT) {
            fs_inode_t *parent = NULL;
            char name[FS_NAME_LEN];
            inode = fs_path_resolve(path, &parent, name, FS_NAME_LEN);
            if (parent == NULL) return -9;  /* NOEXIST */
            int err = fs_dir_create(parent, name, FS_INODE_FILE);
            fs_inode_put(parent);
            if (err != 0) return err;
            inode = fs_store_lookup(path);
            if (inode == NULL) return -9;
        } else {
            return -9;  /* NOEXIST */
        }
    }

    /* 目录不能 open 当文件 */
    if (inode->type == FS_INODE_DIR) {
        fs_inode_put(inode);
        return -14;  /* ISDIR */
    }

    /* O_TRUNC */
    if ((flags & FS_O_TRUNC) && inode->type == FS_INODE_FILE) {
        inode->size = 0;
    }

    int fd = fs_fd_alloc(inode);
    if (fd < 0) {
        fs_inode_put(inode);
        return fd;
    }
    fs_inode_put(inode);  /* fs_fd_alloc 已 get,这里释放 lookup 的引用 */
    return fd;
}

int fs_store_close(int fd) {
    fs_fd_t *f = fs_fd_get(fd);
    if (f == NULL) return -7;  /* CAP */
    fs_fd_free(fd);
    return 0;
}

int32_t fs_store_read(int fd, void *buf, uint32_t len) {
    fs_fd_t *f = fs_fd_get(fd);
    if (f == NULL) return -7;
    if (f->inode->type == FS_INODE_DIR) return -13;  /* NOTDIR... 实际是 ISDIR */

    if (f->inode->type == FS_INODE_FILE) {
        int32_t n = fs_ramfs_read(f->inode, buf, f->offset, len);
        if (n > 0) f->offset += (uint32_t)n;
        return n;
    }
    /* CHRDEV:read 由 fs_server_run 转发给 driver server,store 不直接处理 */
    return -16;  /* NOSYS */
}

int32_t fs_store_write(int fd, const void *buf, uint32_t len) {
    fs_fd_t *f = fs_fd_get(fd);
    if (f == NULL) return -7;
    if (f->inode->type == FS_INODE_DIR) return -14;

    if (f->inode->type == FS_INODE_FILE) {
        int32_t n = fs_ramfs_write(f->inode, buf, f->offset, len);
        if (n > 0) f->offset += (uint32_t)n;
        return n;
    }
    /* CHRDEV:同上,转发 */
    return -16;
}

int fs_store_lseek(int fd, int32_t offset, uint32_t whence) {
    fs_fd_t *f = fs_fd_get(fd);
    if (f == NULL) return -7;
    int32_t newpos = 0;
    if (whence == FS_SEEK_SET) {
        newpos = offset;
    } else if (whence == FS_SEEK_CUR) {
        newpos = (int32_t)f->offset + offset;
    } else if (whence == FS_SEEK_END) {
        newpos = (int32_t)f->inode->size + offset;
    } else {
        return -2;
    }
    if (newpos < 0) return -2;
    f->offset = (uint32_t)newpos;
    return (int)f->offset;
}

int fs_store_readdir(int fd, fs_dirent_t *entry) {
    fs_fd_t *f = fs_fd_get(fd);
    if (f == NULL) return -7;
    if (f->inode->type != FS_INODE_DIR) return -13;  /* NOTDIR */
    int err = fs_dir_readdir(f->inode, f->offset, entry);
    if (err == 0) {
        f->offset++;
    }
    return err;
}

int fs_store_mkdir(const char *path) {
    fs_inode_t *parent = NULL;
    char name[FS_NAME_LEN];
    fs_inode_t *inode = fs_path_resolve(path, &parent, name, FS_NAME_LEN);
    if (inode != NULL) {
        fs_inode_put(inode);
        if (parent) fs_inode_put(parent);
        return -8;  /* BUSY:已存在 */
    }
    if (parent == NULL) return -9;
    int err = fs_dir_create(parent, name, FS_INODE_DIR);
    fs_inode_put(parent);
    return err;
}

int fs_store_unlink(const char *path) {
    fs_inode_t *inode = fs_store_lookup(path);
    if (inode == NULL) return -9;
    if (inode->parent == NULL) {
        fs_inode_put(inode);
        return -12;  /* PERM:不能删根 */
    }
    /* 从父节点摘除 (简化:直接操作链表) */
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
    fs_inode_put(inode);  /* lookup 的引用 */
    fs_inode_put(inode);  /* tree 的引用 (add_child 时 get) */
    return 0;
}

int fs_store_stat(const char *path, fs_statinfo_t *st) {
    fs_inode_t *inode = fs_store_lookup(path);
    if (inode == NULL) return -9;
    st->ino = inode->ino;
    st->size = inode->size;
    st->type = inode->type;
    fs_inode_put(inode);
    return 0;
}

/*============================================================================
 * devfs 设备注册
 *============================================================================*/

int fs_store_register_dev(const char *name, int dev_ep_cap) {
    if (g_dev == NULL || name == NULL) return -2;
    if (fs_inode_lookup_child(g_dev, name) != NULL) return -8;  /* 已存在 */
    fs_inode_t *node = fs_inode_alloc(FS_INODE_CHRDEV, name);
    if (node == NULL) return -4;
    node->dev_ep_cap = dev_ep_cap;
    fs_inode_add_child(g_dev, node);
    return 0;
}

#endif /* VFS_ENABLE && CAP_ENABLE */
