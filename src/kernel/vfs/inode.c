/**
 * @file inode.c
 * @brief inode 静态池管理 + 树操作
 */

#include "inode.h"
#include "mem.h"
#include <string.h>

#if VFS_ENABLE

/*============================================================================
 * 静态 inode 池 + bitmap
 *============================================================================*/

static inode_t inode_pool[MAX_INODES];
static uint32_t inode_bitmap;           /* bit=1 表示使用中 */
static uint32_t ino_counter = 1;        /* 从 1 开始, 0 保留 */

/*============================================================================
 * 池管理
 *============================================================================*/

void inode_init(void) {
    memset(inode_pool, 0, sizeof(inode_pool));
    inode_bitmap = 0;
    ino_counter = 1;
}

static int inode_find_free(void) {
    for (int i = 0; i < MAX_INODES; i++) {
        if (!(inode_bitmap & (1U << i)))
            return i;
    }
    return -1;
}

inode_t *inode_alloc(inode_type_t type, const char *name) {
    int idx = inode_find_free();
    if (idx < 0) return NULL;

    inode_t *inode = &inode_pool[idx];
    memset(inode, 0, sizeof(inode_t));
    inode->ino = ino_counter++;
    strncpy(inode->name, name, INODE_NAME_LEN - 1);
    inode->name[INODE_NAME_LEN - 1] = '\0';
    inode->type = type;
    inode->refcount = 1;

    inode_bitmap |= (1U << idx);
    return inode;
}

void inode_free(inode_t *inode) {
    if (!inode) return;

    /* Force-unlink from parent without refcount cycle */
    if (inode->parent) {
        inode_t *prev = NULL;
        inode_t *c = inode->parent->children;
        while (c) {
            if (c == inode) {
                if (prev) prev->next_sibling = c->next_sibling;
                else inode->parent->children = c->next_sibling;
                break;
            }
            prev = c;
            c = c->next_sibling;
        }
        inode->parent = NULL;
        inode->next_sibling = NULL;
    }

    int idx = (int)(inode - inode_pool);
    if (idx >= 0 && idx < MAX_INODES) {
        inode_bitmap &= ~(1U << idx);
        memset(inode, 0, sizeof(inode_t));
    }
}

/*============================================================================
 * 引用计数
 *============================================================================*/

void inode_get(inode_t *inode) {
    if (inode) inode->refcount++;
}

void inode_put(inode_t *inode) {
    if (!inode) return;
    if (inode->refcount > 0) {
        inode->refcount--;
        if (inode->refcount == 0) {
            inode_free(inode);
        }
    }
}

/*============================================================================
 * 树操作
 *============================================================================*/

void inode_add_child(inode_t *parent, inode_t *child) {
    if (!parent || !child) return;

    child->parent = parent;
    child->next_sibling = NULL;

    if (parent->children == NULL) {
        parent->children = child;
    } else {
        inode_t *sib = parent->children;
        while (sib->next_sibling) {
            sib = sib->next_sibling;
        }
        sib->next_sibling = child;
    }

    inode_get(child);  /* tree holds a reference */
}

inode_t *inode_lookup_child(inode_t *dir, const char *name) {
    if (!dir || !name) return NULL;

    inode_t *child = dir->children;
    while (child) {
        if (strcmp(child->name, name) == 0)
            return child;
        child = child->next_sibling;
    }
    return NULL;
}

kern_err_t inode_remove_child(inode_t *parent, const char *name) {
    if (!parent || !name) return KERN_ERR_PARAM;

    inode_t *prev = NULL;
    inode_t *child = parent->children;

    while (child) {
        if (strcmp(child->name, name) == 0) {
            if (prev) {
                prev->next_sibling = child->next_sibling;
            } else {
                parent->children = child->next_sibling;
            }
            child->parent = NULL;
            child->next_sibling = NULL;
            inode_put(child);  /* release tree's reference */
            return KERN_OK;
        }
        prev = child;
        child = child->next_sibling;
    }
    return KERN_ERR_NOEXIST;
}

#endif /* VFS_ENABLE */
