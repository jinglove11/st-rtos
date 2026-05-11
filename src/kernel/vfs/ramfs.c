/**
 * @file ramfs.c
 * @brief ramfs — 内存文件系统
 */

#include "ramfs.h"
#include "mem.h"
#include <string.h>

#if VFS_ENABLE

/*============================================================================
 * ramfs_data_t — 文件数据 (存在 inode->private_data 中)
 *============================================================================*/

typedef struct {
    uint8_t *buffer;
    uint32_t size;
    uint32_t capacity;
} ramfs_data_t;

#define RAMFS_BLOCK_SIZE 64

/*============================================================================
 * 帮助函数
 *============================================================================*/

static int ramfs_grow(ramfs_data_t *data, uint32_t needed) {
    uint32_t new_cap = data->capacity;
    if (new_cap == 0) new_cap = RAMFS_BLOCK_SIZE;
    while (new_cap < needed)
        new_cap += RAMFS_BLOCK_SIZE;

    uint8_t *new_buf = kmalloc(new_cap);
    if (!new_buf) return -1;

    if (data->buffer && data->size > 0)
        memcpy(new_buf, data->buffer, data->size);

    if (data->buffer) kfree(data->buffer);
    data->buffer = new_buf;
    data->capacity = new_cap;
    return 0;
}

/*============================================================================
 * ramfs 文件操作 (file_ops_t)
 *============================================================================*/

static kern_err_t ramfs_open(inode_t *inode, uint32_t flags) {
    (void)flags;
    if (!inode->private_data) {
        ramfs_data_t *data = kmalloc(sizeof(ramfs_data_t));
        if (!data) return KERN_ERR_RESOURCE;
        memset(data, 0, sizeof(ramfs_data_t));
        inode->private_data = data;
    }
    return KERN_OK;
}

static kern_err_t ramfs_close(inode_t *inode) {
    (void)inode;
    return KERN_OK;
}

static kern_err_t ramfs_truncate(inode_t *inode) {
    ramfs_data_t *data = (ramfs_data_t *)inode->private_data;
    if (!data) return KERN_ERR;
    data->size = 0;
    inode->size = 0;
    return KERN_OK;
}

static int32_t ramfs_read(inode_t *inode, void *buf, uint32_t offset, uint32_t size) {
    ramfs_data_t *data = (ramfs_data_t *)inode->private_data;
    if (!data || !data->buffer) return 0;

    if (offset >= data->size) return 0;
    uint32_t avail = data->size - offset;
    if (size > avail) size = avail;
    memcpy(buf, data->buffer + offset, size);
    return (int32_t)size;
}

static int32_t ramfs_write(inode_t *inode, const void *buf, uint32_t offset, uint32_t size) {
    ramfs_data_t *data = (ramfs_data_t *)inode->private_data;
    if (!data) return KERN_ERR;

    uint32_t needed = offset + size;
    if (needed > data->capacity) {
        if (ramfs_grow(data, needed) < 0) return KERN_ERR_RESOURCE;
    }

    memcpy(data->buffer + offset, buf, size);
    if (needed > data->size) data->size = needed;
    inode->size = data->size;
    return (int32_t)size;
}

static file_ops_t ramfs_file_fops = {
    .open      = ramfs_open,
    .close     = ramfs_close,
    .read      = ramfs_read,
    .write     = ramfs_write,
    .truncate  = ramfs_truncate,
};

/*============================================================================
 * 目录操作 (dir_ops_t)
 *============================================================================*/

static kern_err_t dir_lookup(inode_t *dir, const char *name, inode_t **result) {
    if (!dir || !name || !result) return KERN_ERR_PARAM;

    /* "." → self, ".." → parent */
    if (name[0] == '.') {
        if (name[1] == '\0') {
            *result = dir;
            inode_get(dir);
            return KERN_OK;
        }
        if (name[1] == '.' && name[2] == '\0') {
            *result = dir->parent ? dir->parent : dir;
            inode_get(*result);
            return KERN_OK;
        }
    }

    *result = inode_lookup_child(dir, name);
    return (*result) ? KERN_OK : KERN_ERR_NOEXIST;
}

static kern_err_t dir_create(inode_t *dir, const char *name, uint32_t type) {
    if (!dir || !name) return KERN_ERR_PARAM;

    if (inode_lookup_child(dir, name))
        return KERN_ERR;  /* 同名已存在 */

    inode_t *child = inode_alloc((inode_type_t)type, name);
    if (!child) return KERN_ERR_RESOURCE;

    switch (type) {
    case INODE_TYPE_FILE:
        child->ops_u.file_ops = &ramfs_file_fops;
        break;
    case INODE_TYPE_DIR:
        child->dir_ops = dir->dir_ops;  /* 继承父目录的 dir_ops */
        break;
    default:
        break;
    }

    inode_add_child(dir, child);
    return KERN_OK;
}

static kern_err_t dir_unlink(inode_t *dir, const char *name) {
    if (!dir || !name) return KERN_ERR_PARAM;
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
        return KERN_ERR_PERM;  /* cannot unlink "." or ".." */

    inode_t *child = inode_lookup_child(dir, name);
    if (!child) return KERN_ERR_NOEXIST;

    inode_remove_child(dir, name);
    inode_put(child);  /* release alloc ref; tree ref released by remove_child */
    return KERN_OK;
}

static kern_err_t dir_readdir(inode_t *dir, uint32_t index, dirent_t *entry) {
    if (!dir || !entry) return KERN_ERR_PARAM;

    /* index 0 = ".", index 1 = "..", 2+ = children */
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
    return KERN_ERR;  /* 索引越界 */
}

static dir_ops_t shared_dir_ops = {
    .lookup  = dir_lookup,
    .create  = dir_create,
    .unlink  = dir_unlink,
    .readdir = dir_readdir,
};

/*============================================================================
 * ramfs API
 *============================================================================*/

static inode_t *tmp_root;

void ramfs_init(inode_t *tmp_dir) {
    if (!tmp_dir) return;
    tmp_dir->dir_ops = &shared_dir_ops;
    tmp_root = tmp_dir;
}

inode_t *ramfs_create_file(inode_t *dir, const char *name) {
    if (!dir || !name) return NULL;

    inode_t *inode = inode_alloc(INODE_TYPE_FILE, name);
    if (!inode) return NULL;

    inode->ops_u.file_ops = &ramfs_file_fops;

    ramfs_data_t *data = kmalloc(sizeof(ramfs_data_t));
    if (data) {
        memset(data, 0, sizeof(ramfs_data_t));
        inode->private_data = data;
    }

    inode_add_child(dir, inode);
    return inode;
}

inode_t *ramfs_create_dir(inode_t *dir, const char *name) {
    if (!dir || !name) return NULL;

    inode_t *inode = inode_alloc(INODE_TYPE_DIR, name);
    if (!inode) return NULL;

    inode->dir_ops = &shared_dir_ops;
    inode_add_child(dir, inode);
    return inode;
}

#endif /* VFS_ENABLE */
